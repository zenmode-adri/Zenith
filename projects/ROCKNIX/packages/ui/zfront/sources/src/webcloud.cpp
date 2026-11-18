// MODULE `webcloud`: see webcloud.hpp.
#include "webcloud.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "json.hpp"
#include "oscontract.hpp"
#include "webcore.hpp"
#include "websync.hpp"

namespace zf {
namespace webcloud {

using std::string;

// ---- paths -------------------------------------------------------------------------------------------------------------------

string rclone_path() {
  const char* e = std::getenv("ZF_RCLONE");
  return e && *e ? e : "/usr/bin/rclone";
}
string conf_path() {
  const char* e = std::getenv("ZF_RCLONE_CONF");
  return e && *e ? e : "/storage/.config/rclone/rclone.conf";
}

namespace {

std::mutex g_conf_mu;  // one writer of the configuration at a time

string trim(string s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
  return s.substr(a, b - a);
}

std::vector<string> split_lines(const string& s) {
  std::vector<string> v;
  size_t at = 0;
  while (at <= s.size()) {
    size_t nl = s.find('\n', at);
    if (nl == string::npos) {
      if (at < s.size()) v.push_back(s.substr(at));
      break;
    }
    v.push_back(s.substr(at, nl - at));
    at = nl + 1;
  }
  return v;
}

bool is_header(const string& line, const string& name) { return trim(line) == "[" + name + "]"; }
bool is_any_header(const string& line) {
  const string t = trim(line);
  return !t.empty() && t[0] == '[';
}

string slurp(const string& path) {
  string s;
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return s;
  char b[4096];
  ssize_t n;
  while ((n = ::read(fd, b, sizeof b)) > 0) s.append(b, size_t(n));
  ::close(fd);
  return s;
}

bool write_atomic(const string& path, const string& data) {
  const size_t slash = path.rfind('/');
  if (slash != string::npos && slash > 0) {
    const string dir = path.substr(0, slash);
    ::mkdir(dir.c_str(), 0700);  // the folder may be missing; its parent is /storage/.config
  }
  const string tmp = path + ".zf-tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  size_t off = 0;
  bool ok = true;
  while (off < data.size()) {
    ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n <= 0) {
      ok = false;
      break;
    }
    off += size_t(n);
  }
  ok = ok && ::fsync(fd) == 0;
  ok = (::close(fd) == 0) && ok;
  if (ok) ok = ::rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok) ::unlink(tmp.c_str());
  return ok;
}

}  // namespace

// ---- the INI file --------------------------------------------------------------------------------------------------------------

Remote read_remote(const string& ini) {
  Remote r;
  bool in = false;
  for (const string& l : split_lines(ini)) {
    if (is_any_header(l)) {
      in = is_header(l, kRemote);
      if (in) r.present = true;
      continue;
    }
    if (!in) continue;
    const size_t eq = l.find('=');
    if (eq == string::npos) continue;
    const string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
    if (k == "type") r.type = v;
    else if (k == "url") r.where = v;
    else if (k == "host" && r.where.empty()) r.where = v;
    else if (k == "user") r.user = v;
  }
  if (r.present && r.type != "webdav" && !r.where.empty()) {
    // sftp / ftp: show the port too when there is one
    bool in2 = false;
    for (const string& l : split_lines(ini)) {
      if (is_any_header(l)) {
        in2 = is_header(l, kRemote);
        continue;
      }
      if (!in2) continue;
      const size_t eq = l.find('=');
      if (eq != string::npos && trim(l.substr(0, eq)) == "port") r.where += ":" + trim(l.substr(eq + 1));
    }
  }
  return r;
}

string replace_section(const string& ini, const string& name, const string& body) {
  const std::vector<string> lines = split_lines(ini);
  string out;
  bool skipping = false, done = false;
  auto put_body = [&] {
    if (body.empty() || done) return;
    if (!out.empty() && out.back() != '\n') out += '\n';
    if (!out.empty()) out += '\n';
    out += "[" + name + "]\n" + body;
    if (body.back() != '\n') out += '\n';
    done = true;
  };
  for (const string& l : lines) {
    if (skipping) {
      if (!is_any_header(l)) continue;  // a line of the old section
      skipping = false;
    }
    if (is_header(l, name)) {
      skipping = true;
      put_body();
      continue;
    }
    out += l + "\n";
  }
  put_body();
  return out;
}

bool has_no_section(const string& ini) {
  for (const string& l : split_lines(ini))
    if (is_any_header(l)) return false;
  return true;
}

// ---- fields ----------------------------------------------------------------------------------------------------------------------

bool valid_field(std::string_view v, size_t max) {
  if (v.empty() || v.size() > max) return false;
  for (unsigned char c : v)
    if (c < 0x20 || c == 0x7f) return false;
  return true;
}

bool valid_port(std::string_view v) {
  if (v.empty() || v.size() > 5) return false;
  int n = 0;
  for (char c : v) {
    if (c < '0' || c > '9') return false;
    n = n * 10 + (c - '0');
  }
  return n >= 1 && n <= 65535;
}

bool valid_url(std::string_view v) {
  if (v.size() > 300) return false;
  size_t at = 0;
  if (v.substr(0, 7) == "http://") at = 7;
  else if (v.substr(0, 8) == "https://") at = 8;
  else return false;
  if (v.size() <= at) return false;
  for (unsigned char c : v)
    if (c <= 0x20 || c == 0x7f) return false;
  return true;
}

namespace {

bool valid_host(std::string_view v) {
  if (v.empty() || v.size() > 200) return false;
  for (unsigned char c : v) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == ':' ||
                    c >= 0x80;
    if (!ok) return false;
  }
  return true;
}

string quoted(const string& s) { return "\"" + oscontract::json_escape(s) + "\""; }

}  // namespace

string clean_token(std::string_view raw) {
  // `rclone authorize` prints text around the JSON ("Paste the following into your remote machine --->"): keep what is between the braces
  const size_t a = raw.find('{'), b = raw.rfind('}');
  if (a == std::string_view::npos || b == std::string_view::npos || b < a) return "";
  json::Value v;
  if (!json::parse(raw.substr(a, b - a + 1), v) || !v.is_object()) return "";
  const string access = v.text("access_token"), refresh = v.text("refresh_token");
  // without a refresh token the cloud would stop working within the hour
  if (access.empty() || refresh.empty() || access.size() > 8192 || refresh.size() > 8192) return "";
  string type = v.text("token_type");
  if (type.empty()) type = "Bearer";
  string t = "{\"access_token\":" + quoted(access) + ",\"token_type\":" + quoted(type) + ",\"refresh_token\":" + quoted(refresh);
  const string expiry = v.text("expiry");
  if (!expiry.empty()) t += ",\"expiry\":" + quoted(expiry);
  return t + "}";
}

// ---- running rclone ---------------------------------------------------------------------------------------------------------------

int run_argv(const std::vector<string>& argv, const string& in, int timeout_ms, string& out) {
  out.clear();
  if (argv.empty()) return -1;
  ::signal(SIGPIPE, SIG_IGN);  // a program that exits before reading its input must not take the console down
  int to_child[2], from_child[2];
  if (::pipe2(to_child, O_CLOEXEC) != 0) return -1;
  if (::pipe2(from_child, O_CLOEXEC) != 0) {
    ::close(to_child[0]);
    ::close(to_child[1]);
    return -1;
  }
  std::vector<char*> args;
  for (const string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(to_child[0]);
    ::close(to_child[1]);
    ::close(from_child[0]);
    ::close(from_child[1]);
    return -1;
  }
  if (pid == 0) {
    ::dup2(to_child[0], 0);
    ::dup2(from_child[1], 1);
    ::dup2(from_child[1], 2);
    ::execv(args[0], args.data());
    ::_exit(127);
  }
  ::close(to_child[0]);
  ::close(from_child[1]);
  // the input is short (a password): it fits in the pipe, so this does not block
  size_t off = 0;
  while (off < in.size()) {
    ssize_t n = ::write(to_child[1], in.data() + off, in.size() - off);
    if (n <= 0) break;
    off += size_t(n);
  }
  ::close(to_child[1]);
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  bool timed_out = false;
  for (;;) {
    const int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count());
    if (left <= 0) {
      timed_out = true;
      break;
    }
    pollfd pf{from_child[0], POLLIN, 0};
    const int r = ::poll(&pf, 1, left);
    if (r < 0) continue;
    if (r == 0) {
      timed_out = true;
      break;
    }
    char b[2048];
    const ssize_t n = ::read(from_child[0], b, sizeof b);
    if (n <= 0) break;
    if (out.size() < 65536) out.append(b, size_t(n));
  }
  ::close(from_child[0]);
  if (timed_out) ::kill(pid, SIGKILL);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
  }
  if (timed_out) return -1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
}

namespace {

/// rclone's stored form of a password (`rclone obscure -`, the password on standard input).
bool obscure(const string& pass, string& out, SetResult& fail) {
  if (::access(rclone_path().c_str(), X_OK) != 0) {
    fail = {503, "rclone is not installed on this console"};
    return false;
  }
  string o;
  const int rc = run_argv({rclone_path(), "obscure", "-"}, pass, 10000, o);
  o = trim(o);
  if (rc != 0 || o.empty() || o.find('\n') != string::npos) {
    fail = {500, "could not prepare the password"};
    return false;
  }
  out = o;
  return true;
}

string text_of(const json::Value& v, const char* key) {
  const json::Value* p = v.find(key);
  return p && p->type == json::Value::Type::String ? p->str : string();
}

}  // namespace

// ---- set / remove -----------------------------------------------------------------------------------------------------------------

SetResult set_remote(const string& json_body) {
  json::Value v;
  if (!json::parse(json_body, v) || !v.is_object()) return {400, "invalid request"};
  const string type = text_of(v, "type");
  string body;
  if (type == "webdav") {
    string url = text_of(v, "url");
    const string user = text_of(v, "user"), pass = text_of(v, "pass");
    string vendor = text_of(v, "vendor");
    if (vendor.empty()) vendor = "other";
    if (!url.empty() && url.find("://") == string::npos) url = "https://" + url;  // people type the address without the scheme
    if (!valid_url(url)) return {400, "the address is not valid"};
    if (!valid_field(user, 200) || !valid_field(pass, 300)) return {400, "user name and password are required"};
    if (vendor != "nextcloud" && vendor != "owncloud" && vendor != "other") return {400, "unknown server type"};
    SetResult f;
    string ob;
    if (!obscure(pass, ob, f)) return f;
    body = "type = webdav\nurl = " + url + "\nvendor = " + vendor + "\nuser = " + user + "\npass = " + ob + "\n";
  } else if (type == "sftp" || type == "ftp") {
    const string host = text_of(v, "host"), user = text_of(v, "user"), pass = text_of(v, "pass");
    string port = text_of(v, "port");
    if (port.empty()) port = type == "sftp" ? "22" : "21";
    if (!valid_host(host)) return {400, "the server address is not valid"};
    if (!valid_port(port)) return {400, "the port is not valid"};
    if (!valid_field(user, 200) || !valid_field(pass, 300)) return {400, "user name and password are required"};
    SetResult f;
    string ob;
    if (!obscure(pass, ob, f)) return f;
    body = "type = " + type + "\nhost = " + host + "\nport = " + port + "\nuser = " + user + "\npass = " + ob + "\n";
    if (type == "ftp" && v.boolean("tls")) body += "explicit_tls = true\n";
  } else if (type == "drive" || type == "dropbox") {
    const string token = clean_token(text_of(v, "token"));
    if (token.empty()) return {400, "that code is not valid: paste everything rclone authorize printed"};
    body = "type = " + type + "\n";
    if (type == "drive") body += "scope = drive\n";
    body += "token = " + token + "\n";
  } else {
    return {400, "unknown service"};
  }
  std::lock_guard<std::mutex> lk(g_conf_mu);
  const string path = conf_path();
  if (!write_atomic(path, replace_section(slurp(path), kRemote, body))) return {500, "could not save the configuration"};
  return {};
}

SetResult remove_remote() {
  std::lock_guard<std::mutex> lk(g_conf_mu);
  const string path = conf_path();
  const string ini = slurp(path);
  if (!read_remote(ini).present) return {404, "no cloud is connected"};
  const string rest = replace_section(ini, kRemote, "");
  if (has_no_section(rest)) {
    // nothing else in the file: delete it, because the console shows CLOUD BACKUP as soon as the file exists
    if (::unlink(path.c_str()) != 0) return {500, "could not save the configuration"};
    return {};
  }
  if (!write_atomic(path, rest)) return {500, "could not save the configuration"};
  return {};
}

// ---- the connection test (background) -----------------------------------------------------------------------------------------------

namespace {

struct TestState {
  std::mutex mu;
  std::condition_variable cv;
  bool running = false;
  string state = "idle", message;
};
TestState& ts() {
  static TestState* s = new TestState;  // never destroyed: the thread may outlive main()
  return *s;
}

string last_line(const string& out) {
  string last;
  for (const string& l : split_lines(out)) {
    string t = trim(l);
    if (!t.empty()) last = t;
  }
  for (char& c : last)
    if (static_cast<unsigned char>(c) < 0x20) c = ' ';
  if (last.size() > 160) last.resize(160);
  return last;
}

void run_test() {
  string out;
  if (::access(rclone_path().c_str(), X_OK) != 0) {
    std::lock_guard<std::mutex> lk(ts().mu);
    ts().running = false;
    ts().state = "fail";
    ts().message = "rclone is not installed on this console";
    ts().cv.notify_all();
    return;
  }
  const int rc = run_argv({rclone_path(), "--config", conf_path(), "lsd", string(kRemote) + ":", "--max-depth", "1", "--contimeout", "15s", "--timeout",
                           "20s", "--retries", "1", "--low-level-retries", "1"},
                          "", 45000, out);
  std::lock_guard<std::mutex> lk(ts().mu);
  ts().running = false;
  if (rc == 0) {
    ts().state = "ok";
    ts().message.clear();
  } else {
    ts().state = "fail";
    ts().message = rc < 0 ? "the cloud did not answer in time" : last_line(out);
    if (ts().message.empty()) ts().message = "the connection failed";
  }
  ts().cv.notify_all();
}

}  // namespace

bool wait_idle(int ms) {
  std::unique_lock<std::mutex> lk(ts().mu);
  return ts().cv.wait_for(lk, std::chrono::milliseconds(ms), [] { return !ts().running; });
}

// ---- the routes ---------------------------------------------------------------------------------------------------------------------

namespace {

http::Response status_route() {
  Remote r;
  {
    std::lock_guard<std::mutex> lk(g_conf_mu);
    r = read_remote(slurp(conf_path()));
  }
  string j = string("{\"rclone\":") + (::access(rclone_path().c_str(), X_OK) == 0 ? "true" : "false") + ",\"present\":" + (r.present ? "true" : "false");
  if (r.present) j += ",\"name\":\"" + string(kRemote) + "\",\"type\":" + quoted(r.type) + ",\"where\":" + quoted(r.where) + ",\"user\":" + quoted(r.user);
  return webtransfer::reply(200, j + "}");
}

http::Response test_route(const http::Request& req) {
  if (req.method == "GET") {
    std::lock_guard<std::mutex> lk(ts().mu);
    return webtransfer::reply(200, "{\"state\":\"" + (ts().running ? string("running") : ts().state) + "\",\"message\":" + quoted(ts().message) + "}");
  }
  if (req.method != "POST") return webtransfer::err(405, "method not allowed");
  {
    std::lock_guard<std::mutex> lk(g_conf_mu);
    if (!read_remote(slurp(conf_path())).present) return webtransfer::err(409, "no cloud is connected");
  }
  {
    std::lock_guard<std::mutex> lk(ts().mu);
    if (ts().running) return webtransfer::err(409, "a test is already running");
    ts().running = true;
    ts().state = "running";
    ts().message.clear();
  }
  std::thread(run_test).detach();
  return webtransfer::reply(200, "{\"state\":\"running\"}");
}

http::Response cloud_route(const http::Request& req, const webtransfer::Env&) {
  const string& p = req.path;
  if (p == "/files/cloud") return req.method == "GET" ? status_route() : webtransfer::err(405, "method not allowed");
  if (p == "/files/cloud/test") return test_route(req);
  if (req.method != "POST") return webtransfer::err(405, "method not allowed");
  SetResult r = p == "/files/cloud/set" ? set_remote(req.body) : remove_remote();
  if (r.status != 200) return webtransfer::reply(r.status, "{\"error\":" + quoted(r.error) + "}");
  {
    std::lock_guard<std::mutex> lk(ts().mu);
    if (!ts().running) {
      ts().state = "idle";  // an old answer is no longer true
      ts().message.clear();
    }
  }
  return webtransfer::reply(200, "{\"ok\":true}");
}

}  // namespace

// ---- the page ---------------------------------------------------------------------------------------------------------------------------

ZF_REGISTER_WEB_MODULE([] {
  webtransfer::WebModule m;
  m.name = "cloud";
  m.order = 70;
  m.owns = [](std::string_view p) { return p == "/files/cloud" || p == "/files/cloud/set" || p == "/files/cloud/test" || p == "/files/cloud/remove"; };
  m.route = cloud_route;
  m.tab_id = "cl";
  m.tab_label = "Cloud";
  m.css = R"CSS(#t-cl::before{-webkit-mask-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M18 10h-1.26A8 8 0 1 0 9 20h9a5 5 0 0 0 0-10z'/%3E%3C/svg%3E");mask-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M18 10h-1.26A8 8 0 1 0 9 20h9a5 5 0 0 0 0-10z'/%3E%3C/svg%3E")}
#clf label{display:block;margin-top:.8rem}
#clf textarea{width:100%;min-height:6rem;font:.8rem monospace;padding:.6rem;border-radius:12px;border:1px solid var(--line);background:var(--card2);color:inherit}
#clnote,#clnote2{font-size:.9rem;opacity:.85}#clnote code{display:block;margin:.4rem 0;padding:.4rem .6rem;border-radius:8px;background:var(--card2);word-break:break-all}
#clcon{margin-top:1rem}#clcon button{margin-top:.5rem}#clres.ok{color:var(--ok)}#clres.bad,#clsum .bad{color:var(--bad)}#clsum strong{display:block;margin-bottom:.15rem}#clsum span{overflow-wrap:anywhere})CSS";
  m.section = R"HTML(<p id="clsum"></p>
<div id="clform"><label>Service<select id="clsvc"><option value="webdav">WebDAV (Nextcloud, NAS)</option><option value="sftp">SFTP (server, NAS)</option><option value="ftp">FTP</option><option value="drive">Google Drive (advanced)</option><option value="dropbox">Dropbox (advanced)</option></select></label>
<div id="clf"></div>
<p id="clnote"></p>
<button type="button" id="clsave">Connect</button></div>
<div id="clcon" hidden><button type="button" id="cltest">Test connection</button><button type="button" id="clrm">Disconnect</button><p id="clres"></p></div>
<p id="clnote2">Your saves are copied to a folder called GAMES in the cloud. The console never sends the cloud password back to this page.</p>)HTML";
  m.section += websync::section_html();  // the Syncthing part lives in this tab too (websync.cpp wires it up)
  m.js = R"JS(
var F={},polls=0;
function el(t,c,x){var e=document.createElement(t);if(c)e.className=c;if(x!=null)e.textContent=x;return e}
// Z.call sends no body (its last argument means "download as a file"): a request that carries one is built here
function req(m,path,body,done){var x=new XMLHttpRequest();x.open(m,path);Z.auth(x);
 x.onload=function(){done(x)};x.onerror=function(){Z.say("Cannot reach the console.",true);done(null)};x.send(body)}
function field(key,label,kind,ph,val){var l=el("label");l.appendChild(document.createTextNode(label));var i=el(kind==="area"?"textarea":"input");
 if(kind!=="area")i.type=kind;if(ph)i.placeholder=ph;if(val)i.value=val;i.autocomplete="off";i.autocapitalize="none";i.spellcheck=false;
 l.appendChild(i);Z.$("clf").appendChild(l);F[key]=i;return i}
function note(parts){var n=Z.$("clnote");n.textContent="";parts.forEach(function(p){n.appendChild(p.code?el("code",null,p.code):document.createTextNode(p))})}
function build(){
 var s=Z.$("clsvc").value;Z.$("clf").textContent="";F={};
 if(s==="webdav"){
  field("url","Address","text","cloud.example.com/remote.php/dav/files/NAME/");field("user","User name","text");field("pass","Password","password");
  var l=el("label");l.appendChild(document.createTextNode("Server type"));var sel=el("select");
  [["nextcloud","Nextcloud"],["owncloud","ownCloud"],["other","Other"]].forEach(function(o){var op=el("option",null,o[1]);op.value=o[0];sel.appendChild(op)});
  l.appendChild(sel);Z.$("clf").appendChild(l);F.vendor=sel;
  note(["For Nextcloud or ownCloud use an app password (in the security settings), not your normal one."])}
 else if(s==="sftp"||s==="ftp"){
  field("host","Server","text","server.local");field("port","Port","text","",s==="sftp"?"22":"21");field("user","User name","text");field("pass","Password","password");
  if(s==="ftp"){var c=el("label");var cb=el("input");cb.type="checkbox";c.appendChild(cb);c.appendChild(document.createTextNode("Use encrypted FTP (TLS)"));Z.$("clf").appendChild(c);F.tls=cb}
  note([])}
 else{
  field("token","Code","area","{\"access_token\":...}");
  note(["This service needs you to sign in on a computer that has rclone. There, run this command, sign in in the browser it opens, and paste here everything it prints:",{code:"rclone authorize \""+s+"\""}])}
}
function status(){
 var n=Z.nextReq();
 req("GET","/files/cloud",null,function(x){
  if(!Z.isReq(n)||!x||Z.jerr(x))return;
  var d={};try{d=JSON.parse(x.responseText)}catch(_){return}
  var s=Z.$("clsum");s.textContent="";
  if(!d.rclone){s.appendChild(el("span","bad","rclone is not installed on this console."))}
  else if(d.present){s.appendChild(el("strong",null,"Connected"));s.appendChild(el("span",null,d.type+(d.where?" - "+d.where:"")+(d.user?" - "+d.user:"")))}
  else{s.appendChild(el("span",null,"No cloud connected yet."))}
  Z.$("clcon").hidden=!d.present;
  Z.$("clsave").textContent=d.present?"Replace connection":"Connect"})}
function result(msg,cls){var r=Z.$("clres");r.textContent=msg;r.className=cls||""}
function poll(){
 req("GET","/files/cloud/test",null,function(x){
  if(!x){result("Cannot reach the console.","bad");return}
  var d={};try{d=JSON.parse(x.responseText)}catch(_){}
  if(d.state==="running"&&polls++<60){setTimeout(poll,1000);return}
  if(d.state==="ok"){Z.say("Connected.");result("The connection works.","ok");return}
  Z.say("");  // "Saved. Testing..." stops here: what follows is the result
  if(d.state==="fail"){result("Could not connect: "+(d.message||""),"bad");return}  // the line rclone ended with, as it is, after the sentence
  result("Testing took too long.","bad")})}
function test(){result("Testing...");polls=0;
 req("POST","/files/cloud/test","",function(x){if(!x)return;if(x.status===409){poll();return}if(Z.jerr(x))return;setTimeout(poll,1000)})}
Z.$("clsvc").onchange=build;
Z.$("clsave").onclick=function(){
 var b={type:Z.$("clsvc").value},empty=false;
 for(var k in F){var f=F[k];if(f.type==="checkbox"){b[k]=f.checked;continue}var v=k==="pass"?f.value:f.value.trim();b[k]=v;if(!v&&k!=="vendor"&&k!=="port")empty=true}
 if(empty){Z.say("Fill in all the fields.",true);return}
 Z.say("Saving...");
 req("POST","/files/cloud/set",JSON.stringify(b),function(x){
  if(!x)return;if(Z.jerr(x))return;
  Z.say("Saved. Testing...");for(var k in F)if(F[k].type==="password"||F[k].tagName==="TEXTAREA")F[k].value="";
  status();test()})};
Z.$("cltest").onclick=test;
Z.$("clrm").onclick=function(){
 if(!confirm("Disconnect from the cloud?"))return;
 req("POST","/files/cloud/remove","",function(x){if(!x)return;if(Z.jerr(x))return;Z.say("Disconnected.");result("");status()})};
build();
Z.ontab.cl=status;
// with many tabs the bar scrolls: the one just chosen is brought into view, so it is never left cut at the edge
Z.$("tabs").addEventListener("click",function(e){var b=e.target.closest("button");if(b&&b.scrollIntoView)b.scrollIntoView({inline:"center",block:"nearest"})});
)JS";
  return m;
});

}  // namespace webcloud
}  // namespace zf
