// webcloud: the Cloud tab. The real httpd server on 127.0.0.1 (port 0), a FAKE rclone (a shell script that logs its arguments and answers
// `obscure -` and `lsd` as the test says) and a rclone.conf in a temp dir. Nothing touches /storage and no network is used.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "check.hpp"
#include "httpd.hpp"
#include "webcloud.hpp"
#include "webcore.hpp"
#include "webtransfer.hpp"

using namespace zf;
using std::string;

namespace {

const char* kPin = "4821";

int connect_to(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

string talk(uint16_t port, const string& raw) {
  int fd = connect_to(port);
  if (fd < 0) return "";
  size_t off = 0;
  while (off < raw.size()) {
    ssize_t n = ::send(fd, raw.data() + off, raw.size() - off, MSG_NOSIGNAL);
    if (n <= 0) break;
    off += size_t(n);
  }
  string out;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(6000);
  for (;;) {
    int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count());
    if (left <= 0) break;
    pollfd pf{fd, POLLIN, 0};
    if (::poll(&pf, 1, left) <= 0) break;
    char b[2048];
    ssize_t n = ::recv(fd, b, sizeof b, 0);
    if (n <= 0) break;
    out.append(b, size_t(n));
  }
  ::close(fd);
  return out;
}

int status_of(const string& r) { return r.compare(0, 9, "HTTP/1.1 ") == 0 && r.size() >= 12 ? std::atoi(r.c_str() + 9) : 0; }
string body_of(const string& r) {
  size_t i = r.find("\r\n\r\n");
  return i == string::npos ? string() : r.substr(i + 4);
}

string get(uint16_t port, const string& path, const char* pin = kPin) {
  string h = "GET " + path + " HTTP/1.1\r\nHost: x\r\n";
  if (pin) h += string("X-PIN: ") + pin + "\r\n";
  return talk(port, h + "\r\n");
}
string post(uint16_t port, const string& path, const string& body, const char* pin = kPin) {
  string h = "POST " + path + " HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  if (pin) h += string("X-PIN: ") + pin + "\r\n";
  return talk(port, h + "\r\n" + body);
}

string slurp(const string& p) {
  string s;
  if (FILE* f = std::fopen(p.c_str(), "rb")) {
    char b[4096];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
  }
  return s;
}
void write_file(const string& p, const string& data, mode_t mode = 0644) {
  if (FILE* f = std::fopen(p.c_str(), "wb")) {
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
  }
  ::chmod(p.c_str(), mode);
}
bool exists(const string& p) {
  struct stat sb;
  return ::lstat(p.c_str(), &sb) == 0;
}
bool has(const string& hay, const string& needle) { return hay.find(needle) != string::npos; }

// The fake rclone: `obscure -` prints OB:<stdin>; `lsd` answers by the word in <root>/mode (ok / fail / slow); every call is logged.
const char* kFake = R"SH(#!/bin/sh
D="$(dirname "$0")"
echo "$@" >> "$D/argv.log"
if [ "$1" = "obscure" ]; then p=$(cat); echo "OB:$p"; exit 0; fi
for a in "$@"; do
  if [ "$a" = "lsd" ]; then
    case "$(cat "$D/mode" 2>/dev/null)" in
      fail) echo "NOTICE: noise"; echo "Failed to create file system: login failed for user"; exit 1 ;;
      slow) sleep 2; exit 0 ;;
      *) echo "          -1 2026-01-01 00:00:00        -1 GAMES"; exit 0 ;;
    esac
  fi
done
exit 0
)SH";

struct Rig {
  string root, conf;
  http::Server srv;
  webtransfer::Env env;
  Rig() {
    std::string t = zt::tmp_base() + "/zf-webcloud-XXXXXX";
    root = ::mkdtemp(&t[0]);
    conf = root + "/cfg/rclone.conf";
    write_file(root + "/rclone", kFake, 0755);
    ::setenv("ZF_RCLONE", (root + "/rclone").c_str(), 1);
    ::setenv("ZF_RCLONE_CONF", conf.c_str(), 1);
    webtransfer::reset_for_test(kPin);
    webtransfer::set_enabled(true);  // the switch of the console's menu: off, every route answers 404
    string err;
    bool ok = srv.start(
        0,
        [this](const http::Request& r) {
          if (r.path.compare(0, 6, "/files") == 0) return webtransfer::route(r, env);
          http::Response x;
          x.status = 404;
          return x;
        },
        err);
    CHECK(ok);
  }
  ~Rig() {
    srv.stop();
    webcloud::wait_idle(5000);
    string cmd = "rm -rf '" + root + "'";
    if (std::system(cmd.c_str()) != 0) {
    }
  }
  uint16_t port() const { return srv.port(); }
  void mode(const char* m) { write_file(root + "/mode", m); }
  string argv() { return slurp(root + "/argv.log"); }
};

const char* kWebdav = "{\"type\":\"webdav\",\"url\":\"https://cloud.example.com/remote.php/dav/files/me/\",\"user\":\"me\",\"pass\":\"sec ret\",\"vendor\":\"nextcloud\"}";

void pure() {
  using namespace webcloud;
  CHECK(valid_port("22") && valid_port("65535") && !valid_port("0") && !valid_port("65536") && !valid_port("") && !valid_port("2a") && !valid_port("123456"));
  CHECK(valid_url("https://a.b/c") && valid_url("http://x") && !valid_url("ftp://x") && !valid_url("https://") && !valid_url("https://a b") &&
        !valid_url("javascript:alert(1)"));
  CHECK(valid_field("abc", 5) && !valid_field("", 5) && !valid_field("abcdef", 5) && !valid_field("a\nb", 5) && !valid_field(string("a\0b", 3), 5) &&
        valid_field("a b'\"$(x)", 20));
  // the section is replaced in place, the others are kept; an empty body removes it
  const string ini = "[a]\ntype = x\n\n[zenith]\ntype = old\nuser = u\n\n[b]\ntype = y\n";
  const string r = replace_section(ini, "zenith", "type = new\n");
  CHECK(has(r, "[a]\ntype = x") && has(r, "[zenith]\ntype = new\n") && has(r, "[b]\ntype = y") && !has(r, "old") && !has(r, "user = u"));
  CHECK(!has(replace_section(ini, "zenith", ""), "zenith") && has(replace_section(ini, "zenith", ""), "[b]"));
  CHECK(has(replace_section("", "zenith", "type = n\n"), "[zenith]\ntype = n\n"));
  CHECK(has(replace_section("[a]\ntype = x\n", "zenith", "type = n\n"), "[a]\ntype = x\n\n[zenith]\ntype = n\n"));
  CHECK(has_no_section("") && has_no_section("# nothing\n") && !has_no_section("[a]\n"));
  Remote m = read_remote("[zenith]\ntype = sftp\nhost = h.example\nport = 2222\nuser = me\npass = zz\n");
  CHECK(m.present && m.type == "sftp" && m.where == "h.example:2222" && m.user == "me");
  CHECK(!read_remote("[other]\ntype = x\n").present);
  // the code that `rclone authorize` prints, with the text around it
  const string wrapped = "Paste the following into your remote machine --->\n{\"access_token\":\"AT\",\n \"token_type\":\"Bearer\",\"refresh_token\":\"RT\",\"expiry\":\"2026-01-01T00:00:00Z\"}\n<---End paste\n";
  const string t = clean_token(wrapped);
  CHECK(t == "{\"access_token\":\"AT\",\"token_type\":\"Bearer\",\"refresh_token\":\"RT\",\"expiry\":\"2026-01-01T00:00:00Z\"}");
  CHECK(clean_token("hello").empty() && clean_token("{}").empty() && clean_token("{\"access_token\":\"AT\"}").empty());
  CHECK(clean_token("{\"access_token\":\"A\\\"T\",\"refresh_token\":\"R\"}").find('\n') == string::npos);
  // a program with no shell: arguments are not interpreted; a slow one is killed
  string out;
  CHECK(run_argv({"/bin/echo", "$(touch /tmp/zf-webcloud-pwned)", ";", "x"}, "", 3000, out) == 0 && has(out, "$(touch"));
  CHECK(!exists("/tmp/zf-webcloud-pwned"));
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(run_argv({"/bin/sleep", "5"}, "", 300, out) == -1);
  CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
  CHECK(run_argv({"/nonexistent/prog"}, "", 1000, out) == 127);
  CHECK(run_argv({"/bin/cat"}, "from stdin", 3000, out) == 0 && out == "from stdin");
}

void flow() {
  Rig r;
  const uint16_t p = r.port();
  // not allowed without the PIN
  CHECK(status_of(get(p, "/files/cloud", nullptr)) == 403);
  CHECK(status_of(post(p, "/files/cloud/set", kWebdav, nullptr)) == 403);
  CHECK(!exists(r.conf));
  string g = get(p, "/files/cloud");
  CHECK(status_of(g) == 200 && has(body_of(g), "\"rclone\":true") && has(body_of(g), "\"present\":false"));
  CHECK(status_of(post(p, "/files/cloud/test", "")) == 409);  // nothing to test yet
  CHECK(status_of(post(p, "/files/cloud/remove", "")) == 404);

  // WebDAV: saved, mode 0600, password obscured through stdin only, nothing secret comes back
  string s = post(p, "/files/cloud/set", kWebdav);
  CHECK(status_of(s) == 200);
  const string ini = slurp(r.conf);
  CHECK(has(ini, "[zenith]\ntype = webdav\nurl = https://cloud.example.com/remote.php/dav/files/me/\nvendor = nextcloud\nuser = me\npass = OB:sec ret\n"));
  struct stat sb;
  CHECK(::stat(r.conf.c_str(), &sb) == 0 && (sb.st_mode & 0777) == 0600);
  CHECK(!exists(r.conf + ".zf-tmp"));
  CHECK(!has(r.argv(), "sec ret"));  // never an argument
  g = get(p, "/files/cloud");
  CHECK(has(body_of(g), "\"present\":true") && has(body_of(g), "\"type\":\"webdav\"") && has(body_of(g), "\"user\":\"me\""));
  CHECK(!has(g, "sec ret") && !has(g, "OB:"));

  // the connection test runs in the background
  r.mode("ok");
  CHECK(status_of(post(p, "/files/cloud/test", "")) == 200);
  CHECK(webcloud::wait_idle(8000));
  CHECK(has(body_of(get(p, "/files/cloud/test")), "\"state\":\"ok\""));
  CHECK(has(r.argv(), "lsd zenith: --max-depth 1") && has(r.argv(), "--config " + r.conf));
  r.mode("fail");
  CHECK(status_of(post(p, "/files/cloud/test", "")) == 200);
  CHECK(webcloud::wait_idle(8000));
  string t = body_of(get(p, "/files/cloud/test"));
  CHECK(has(t, "\"state\":\"fail\"") && has(t, "Failed to create file system: login failed for user") && !has(t, "NOTICE"));
  r.mode("slow");
  CHECK(status_of(post(p, "/files/cloud/test", "")) == 200);
  CHECK(has(body_of(get(p, "/files/cloud/test")), "\"state\":\"running\""));
  CHECK(status_of(post(p, "/files/cloud/test", "")) == 409);  // one at a time
  CHECK(webcloud::wait_idle(8000));

  // replacing the connection keeps other remotes of the file; removing keeps them too
  write_file(r.conf, "[mine]\ntype = s3\n\n" + slurp(r.conf), 0600);
  CHECK(status_of(post(p, "/files/cloud/set", "{\"type\":\"sftp\",\"host\":\"nas.local\",\"user\":\"me\",\"pass\":\"pa'ss\\\"$(x)\"}")) == 200);
  const string ini2 = slurp(r.conf);
  CHECK(has(ini2, "[mine]\ntype = s3") && has(ini2, "[zenith]\ntype = sftp\nhost = nas.local\nport = 22\nuser = me\npass = OB:pa'ss\"$(x)\n") && !has(ini2, "webdav"));
  CHECK(status_of(post(p, "/files/cloud/remove", "")) == 200);
  CHECK(exists(r.conf) && has(slurp(r.conf), "[mine]") && !has(slurp(r.conf), "zenith"));
  CHECK(has(body_of(get(p, "/files/cloud")), "\"present\":false"));
  // with nothing else left the file goes away (the console shows CLOUD BACKUP as soon as it exists)
  CHECK(status_of(post(p, "/files/cloud/set", kWebdav)) == 200);
  write_file(r.conf, "[zenith]\ntype = webdav\n", 0600);
  CHECK(status_of(post(p, "/files/cloud/remove", "")) == 200);
  CHECK(!exists(r.conf));

  // FTP with TLS, Drive and Dropbox with a pasted code
  CHECK(status_of(post(p, "/files/cloud/set", "{\"type\":\"ftp\",\"host\":\"f.example\",\"port\":\"2121\",\"user\":\"u\",\"pass\":\"p\",\"tls\":true}")) == 200);
  CHECK(has(slurp(r.conf), "type = ftp\nhost = f.example\nport = 2121\nuser = u\npass = OB:p\nexplicit_tls = true\n"));
  const string tok = "Paste the following into your remote machine --->\\n{\\\"access_token\\\":\\\"AT\\\",\\\"token_type\\\":\\\"Bearer\\\",\\\"refresh_token\\\":\\\"RT\\\"}\\n<---End paste";
  CHECK(status_of(post(p, "/files/cloud/set", "{\"type\":\"webdav\",\"url\":\"dav.example.org/x/\",\"user\":\"u\",\"pass\":\"p\"}")) == 200);  // no scheme: https is assumed
  CHECK(has(slurp(r.conf), "url = https://dav.example.org/x/\n"));
  CHECK(status_of(post(p, "/files/cloud/set", "{\"type\":\"drive\",\"token\":\"" + tok + "\"}")) == 200);
  const string d = slurp(r.conf);
  CHECK(has(d, "[zenith]\ntype = drive\nscope = drive\ntoken = {\"access_token\":\"AT\",\"token_type\":\"Bearer\",\"refresh_token\":\"RT\"}\n") && !has(d, "ftp"));
  CHECK(status_of(post(p, "/files/cloud/set", "{\"type\":\"dropbox\",\"token\":\"" + tok + "\"}")) == 200);
  CHECK(has(slurp(r.conf), "type = dropbox\ntoken = {") && !has(slurp(r.conf), "scope"));
  CHECK(has(body_of(get(p, "/files/cloud")), "\"type\":\"dropbox\""));

  // what is refused: nothing is written
  const string before = slurp(r.conf);
  for (const char* bad : {"not json", "[]", "{\"type\":\"nope\"}", "{\"type\":\"webdav\",\"url\":\"ftp://x\",\"user\":\"u\",\"pass\":\"p\"}",
                          "{\"type\":\"webdav\",\"url\":\"https://x\",\"user\":\"\",\"pass\":\"p\"}", "{\"type\":\"webdav\",\"url\":\"https://x\",\"user\":\"u\",\"pass\":\"p\",\"vendor\":\"evil\"}",
                          "{\"type\":\"sftp\",\"host\":\"h\",\"port\":\"70000\",\"user\":\"u\",\"pass\":\"p\"}", "{\"type\":\"sftp\",\"host\":\"h h\",\"user\":\"u\",\"pass\":\"p\"}",
                          "{\"type\":\"sftp\",\"host\":\"h\",\"user\":\"u\\nx = y\",\"pass\":\"p\"}", "{\"type\":\"sftp\",\"host\":\"h\",\"user\":\"u\",\"pass\":\"\"}",
                          "{\"type\":\"drive\",\"token\":\"garbage\"}", "{\"type\":\"drive\",\"token\":\"{\\\"access_token\\\":\\\"A\\\"}\"}"})
    CHECK(status_of(post(p, "/files/cloud/set", bad)) == 400);
  CHECK(slurp(r.conf) == before);
  CHECK(status_of(get(p, "/files/cloud/set")) == 405 && status_of(post(p, "/files/cloud", "")) == 405);

  // the page has its tab, and no secret of ours in it
  const string page = webtransfer::page_html();
  CHECK(has(page, "id=\"t-cl\"") && has(page, "id=\"clsave\"") && has(page, "/files/cloud/set"));
}

void without_rclone() {
  Rig r;
  ::setenv("ZF_RCLONE", (r.root + "/nope").c_str(), 1);
  string g = get(r.port(), "/files/cloud");
  CHECK(has(body_of(g), "\"rclone\":false"));
  CHECK(status_of(post(r.port(), "/files/cloud/set", kWebdav)) == 503);
  CHECK(!exists(r.conf));
  // a code needs no rclone to be saved
  CHECK(status_of(post(r.port(), "/files/cloud/set", "{\"type\":\"dropbox\",\"token\":\"{\\\"access_token\\\":\\\"A\\\",\\\"refresh_token\\\":\\\"R\\\"}\"}")) == 200);
  CHECK(status_of(post(r.port(), "/files/cloud/test", "")) == 200);
  CHECK(webcloud::wait_idle(8000));
  CHECK(has(body_of(get(r.port(), "/files/cloud/test")), "rclone is not installed"));
}

}  // namespace

int main() {
  pure();
  flow();
  without_rclone();
  std::printf("test_webcloud: ok\n");
  return zt::done();
}
