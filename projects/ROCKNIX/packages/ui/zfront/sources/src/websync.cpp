// MODULE `websync`: see websync.hpp.
#include "websync.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <mutex>
#include <set>
#include <vector>

#include "json.hpp"
#include "oscontract.hpp"
#include "webcloud.hpp"
#include "webcore.hpp"

namespace zf {
namespace websync {

using std::string;

namespace {

string env_or(const char* name, const char* def) {
  const char* e = std::getenv(name);
  return e && *e ? e : def;
}

std::mutex g_mu;  // one change of Syncthing's configuration at a time

string quoted(const string& s) { return "\"" + oscontract::json_escape(s) + "\""; }

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

/// A value inside a curl configuration line: backslash and quote escaped (the body is one line of JSON, so there is nothing else to escape).
string curl_escape(const string& s) {
  string o;
  for (char c : s) {
    if (c == '\\' || c == '"') o += '\\';
    o += c;
  }
  return o;
}

void make_dirs(const string& path) {
  for (size_t at = 1; at <= path.size(); ++at)
    if (at == path.size() || path[at] == '/') ::mkdir(path.substr(0, at).c_str(), 0755);
}

bool name_ok(const string& v, size_t max) {
  if (v.empty() || v.size() > max) return false;
  for (unsigned char c : v)
    if (c < 0x20 || c == 0x7f) return false;
  return true;
}

const Preset kPresets[] = {
    {"saves", "Zenith saves", "/.config/retroarch/saves", false},
    {"states", "Zenith save states", "/.config/retroarch/states", false},
    {"screenshots", "Zenith screenshots", "/roms/screenshots", true},
};

string folder_id(const Preset& p) { return string("zenith-") + p.key; }

/// This console's own code ("" when Syncthing does not answer).
string own_id() {
  Reply r = rest("GET", "/rest/system/status", "");
  if (r.status != 200) return "";
  json::Value v;
  if (!json::parse(r.body, v) || !v.is_object()) return "";
  return v.text("myID");
}

http::Response down() { return webtransfer::err(503, "Syncthing is not running"); }

// ---- GET /files/sync ----------------------------------------------------------------------------------------------------------

http::Response status_route() {
  const bool installed = ::access(syncthing_path().c_str(), X_OK) == 0;
  string j = string("{\"installed\":") + (installed ? "true" : "false");
  const string me = installed ? own_id() : string();
  if (me.empty()) return webtransfer::reply(200, j + ",\"running\":false}");
  j += ",\"running\":true,\"myid\":" + quoted(me);

  std::set<string> up;  // devices connected right now
  Reply c = rest("GET", "/rest/system/connections", "");
  json::Value cv;
  if (c.status == 200 && json::parse(c.body, cv)) {
    if (const json::Value* all = cv.find("connections"))
      for (const auto& kv : all->obj)
        if (kv.second.boolean("connected")) up.insert(kv.first);
  }

  j += ",\"devices\":[";
  Reply d = rest("GET", "/rest/config/devices", "");
  json::Value dv;
  bool first = true;
  if (d.status == 200 && json::parse(d.body, dv) && dv.is_array()) {
    for (const json::Value& x : dv.arr) {
      const string id = x.text("deviceID");
      if (id.empty() || id == me) continue;
      if (!first) j += ",";
      first = false;
      j += "{\"id\":" + quoted(id) + ",\"name\":" + quoted(x.text("name")) + ",\"connected\":" + (up.count(id) ? "true" : "false") + "}";
    }
  }
  j += "],\"folders\":[";
  Reply f = rest("GET", "/rest/config/folders", "");
  json::Value fv;
  first = true;
  if (f.status == 200 && json::parse(f.body, fv) && fv.is_array()) {
    for (const json::Value& x : fv.arr) {
      const string id = x.text("id");
      const Preset* p = nullptr;
      for (const Preset& q : kPresets)
        if (id == folder_id(q)) p = &q;
      if (!p) continue;  // only the folders this page made
      if (!first) j += ",";
      first = false;
      j += "{\"preset\":" + quoted(p->key) + ",\"label\":" + quoted(x.text("label")) + ",\"devices\":[";
      bool f2 = true;
      if (const json::Value* ds = x.find("devices"))
        for (const json::Value& dd : ds->arr) {
          const string did = dd.text("deviceID");
          if (did.empty() || did == me) continue;
          if (!f2) j += ",";
          f2 = false;
          j += quoted(did);
        }
      j += "]}";
    }
  }
  return webtransfer::reply(200, j + "]}");
}

// ---- the changes ---------------------------------------------------------------------------------------------------------------

bool run_service(bool on) {
  string out;
  const char* fake = std::getenv("ZF_SYNC_SERVICE");
  if (fake && *fake) return webcloud::run_argv({fake, on ? "start" : "stop"}, "", 8000, out) == 0;
  // fixed text, nothing from the page in it; the choice is kept for the next boot like the console's own switch does
  const char* cmd = on ? ". /etc/profile; set_setting syncthing.enabled 1; systemctl --no-block start syncthing"
                       : ". /etc/profile; set_setting syncthing.enabled 0; systemctl --no-block stop syncthing";
  return webcloud::run_argv({"/bin/sh", "-c", cmd}, "", 8000, out) == 0;
}

/// Syncthing's answer to a change: 200 / 204 is done; no answer is "not running"; anything else is a refusal.
http::Response verdict(const Reply& r) {
  if (r.status == 0) return down();
  if (r.status == 200 || r.status == 204) return webtransfer::reply(200, "{\"ok\":true}");
  return webtransfer::err(502, "Syncthing did not accept that");
}

bool read_json(const http::Request& req, json::Value& v) { return json::parse(req.body, v) && v.is_object(); }

http::Response device_add(const http::Request& req) {
  json::Value v;
  if (!read_json(req, v)) return webtransfer::err(400, "invalid request");
  string id;
  if (!normalise_device_id(v.text("id"), id)) return webtransfer::err(400, "that device code is not valid");
  string name = v.text("name");
  if (name.empty()) name = "Device";
  if (!name_ok(name, 40)) return webtransfer::err(400, "that name is not valid");
  std::lock_guard<std::mutex> lk(g_mu);
  const string me = own_id();
  if (me.empty()) return down();
  if (id == me) return webtransfer::err(400, "that is the code of this console");
  return verdict(rest("PUT", "/rest/config/devices/" + id,
                      "{\"deviceID\":" + quoted(id) + ",\"name\":" + quoted(name) + ",\"addresses\":[\"dynamic\"],\"autoAcceptFolders\":false}"));
}

http::Response device_remove(const http::Request& req) {
  json::Value v;
  if (!read_json(req, v)) return webtransfer::err(400, "invalid request");
  string id;
  if (!normalise_device_id(v.text("id"), id)) return webtransfer::err(400, "that device code is not valid");
  std::lock_guard<std::mutex> lk(g_mu);
  const string me = own_id();
  if (me.empty()) return down();
  if (id == me) return webtransfer::err(400, "that is the code of this console");
  return verdict(rest("DELETE", "/rest/config/devices/" + id, ""));
}

http::Response share(const http::Request& req) {
  json::Value v;
  if (!read_json(req, v)) return webtransfer::err(400, "invalid request");
  const Preset* p = find_preset(v.text("preset"));
  if (!p) return webtransfer::err(400, "unknown folder");
  string dev;
  if (!normalise_device_id(v.text("device"), dev)) return webtransfer::err(400, "that device code is not valid");
  std::lock_guard<std::mutex> lk(g_mu);
  const string me = own_id();
  if (me.empty()) return down();
  // only a device that is already added can receive a folder
  Reply d = rest("GET", "/rest/config/devices", "");
  json::Value dv;
  bool known = false;
  if (d.status == 200 && json::parse(d.body, dv) && dv.is_array())
    for (const json::Value& x : dv.arr)
      if (x.text("deviceID") == dev) known = true;
  if (!known || dev == me) return webtransfer::err(400, "add that device first");
  // the devices the folder is already shared with stay
  std::vector<string> ids{me};
  Reply cur = rest("GET", "/rest/config/folders/" + folder_id(*p), "");
  json::Value cv;
  if (cur.status == 200 && json::parse(cur.body, cv))
    if (const json::Value* ds = cv.find("devices"))
      for (const json::Value& x : ds->arr) {
        const string id = x.text("deviceID");
        bool dup = false;
        for (const string& s : ids) dup = dup || s == id;
        if (!id.empty() && !dup) ids.push_back(id);
      }
  bool dup = false;
  for (const string& s : ids) dup = dup || s == dev;
  if (!dup) ids.push_back(dev);
  const string path = root_path() + p->rel;
  make_dirs(path);  // Syncthing refuses a folder whose path does not exist
  string devs;
  for (const string& s : ids) devs += (devs.empty() ? "" : ",") + string("{\"deviceID\":") + quoted(s) + "}";
  return verdict(rest("PUT", "/rest/config/folders/" + folder_id(*p),
                      "{\"id\":" + quoted(folder_id(*p)) + ",\"label\":" + quoted(p->label) + ",\"path\":" + quoted(path) + ",\"type\":\"" +
                          (p->send_only ? "sendonly" : "sendreceive") + "\",\"devices\":[" + devs + "],\"fsWatcherEnabled\":true}"));
}

http::Response unshare(const http::Request& req) {
  json::Value v;
  if (!read_json(req, v)) return webtransfer::err(400, "invalid request");
  const Preset* p = find_preset(v.text("preset"));
  if (!p) return webtransfer::err(400, "unknown folder");
  std::lock_guard<std::mutex> lk(g_mu);
  return verdict(rest("DELETE", "/rest/config/folders/" + folder_id(*p), ""));
}

http::Response power(const http::Request& req) {
  json::Value v;
  if (!read_json(req, v)) return webtransfer::err(400, "invalid request");
  if (::access(syncthing_path().c_str(), X_OK) != 0) return webtransfer::err(503, "Syncthing is not installed on this console");
  if (!run_service(v.boolean("on"))) return webtransfer::err(502, "could not change Syncthing");
  return webtransfer::reply(200, "{\"ok\":true}");
}

http::Response sync_route(const http::Request& req, const webtransfer::Env&) {
  const string& p = req.path;
  if (p == "/files/sync") return req.method == "GET" ? status_route() : webtransfer::err(405, "method not allowed");
  if (req.method != "POST") return webtransfer::err(405, "method not allowed");
  if (p == "/files/sync/power") return power(req);
  if (p == "/files/sync/device") return device_add(req);
  if (p == "/files/sync/device/remove") return device_remove(req);
  if (p == "/files/sync/share") return share(req);
  return unshare(req);  // /files/sync/unshare
}

}  // namespace

// ---- public pieces -----------------------------------------------------------------------------------------------------------------

string conf_path() { return env_or("ZF_SYNC_CONF", "/storage/.config/syncthing/config.xml"); }
string base_url() { return env_or("ZF_SYNC_URL", "http://127.0.0.1:8384"); }
string root_path() { return env_or("ZF_SYNC_ROOT", "/storage"); }
string syncthing_path() { return env_or("ZF_SYNCTHING", "/usr/bin/syncthing"); }
string curl_path() { return env_or("ZF_CURL", "/usr/bin/curl"); }

string api_key_from(const string& xml) {
  const size_t a = xml.find("<apikey>");
  if (a == string::npos) return "";
  const size_t from = a + 8, b = xml.find("</apikey>", from);
  if (b == string::npos) return "";
  const string k = xml.substr(from, b - from);
  if (k.empty() || k.size() > 128) return "";
  for (unsigned char c : k)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return "";
  return k;  // plain characters only: it goes inside a quoted curl setting
}

bool normalise_device_id(const string& in, string& out) {
  string s;
  for (char c : in) {
    if (c == ' ' || c == '-' || c == '\t') continue;
    s += char(c >= 'a' && c <= 'z' ? c - 32 : c);
  }
  if (s.size() != 56) return false;
  for (char c : s)
    if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) return false;
  out.clear();
  for (size_t i = 0; i < s.size(); i += 7) out += (i ? "-" : "") + s.substr(i, 7);
  return true;
}

const Preset* find_preset(const string& key) {
  for (const Preset& p : kPresets)
    if (key == p.key) return &p;
  return nullptr;
}

Reply rest(const string& method, const string& path, const string& body) {
  const string key = api_key_from(slurp(conf_path()));
  if (key.empty()) return {};
  string cfg = "url = \"" + base_url() + path + "\"\nrequest = \"" + method + "\"\nheader = \"X-API-Key: " + key + "\"\n";
  if (!body.empty()) cfg += "header = \"Content-Type: application/json\"\ndata = \"" + curl_escape(body) + "\"\n";
  string out;
  const int rc = webcloud::run_argv({curl_path(), "-sS", "--max-time", "3", "-w", "\n%{http_code}", "--config", "-"}, cfg, 5000, out);
  const size_t nl = out.rfind('\n');
  Reply r;
  if (rc < 0 || nl == string::npos) return r;
  r.status = std::atoi(out.c_str() + nl + 1);
  r.body = out.substr(0, nl);
  return r;
}

string section_html() {
  return R"HTML(
<div id="sy" hidden>
<h3 id="syh">Sync with a computer or phone</h3>
<p id="sysum"></p>
<div id="syoff" hidden><p>Syncthing keeps folders the same on several devices of yours, with no cloud in between.</p><button type="button" id="syon">Turn on</button></div>
<div id="syact" hidden>
<p>The code of this console:</p><code id="symy"></code><button type="button" id="sycopy">Copy code</button>
<p>On your computer or phone, install Syncthing, add a device and type this code. Then add the code of that device here.</p>
<label>Code of the other device<input id="sycode" type="text" autocomplete="off" autocapitalize="characters" spellcheck="false"></label>
<label>Name of the device<input id="syname" type="text" autocomplete="off" maxlength="40" placeholder="My computer"></label>
<button type="button" id="syadd">Add device</button>
<div id="sydevs"></div>
<label>What to share<select id="sywhat"><option value="saves">Saves</option><option value="states">Save states</option><option value="screenshots">Screenshots (the console only sends)</option></select></label>
<label>With<select id="sywith"></select></label>
<button type="button" id="syshare">Share this folder</button>
<div id="syfolders"></div>
<p>The other device has to accept the folder the first time.</p>
<button type="button" id="syoffbtn">Turn off</button>
</div>
</div>)HTML";
}

}  // namespace websync

// ---- the page ---------------------------------------------------------------------------------------------------------------------------

namespace {

const char* kSyncJs = R"JS(
function el(t,c,x){var e=document.createElement(t);if(c)e.className=c;if(x!=null)e.textContent=x;return e}
function req(m,path,body,done){var x=new XMLHttpRequest();x.open(m,path);Z.auth(x);
 x.onload=function(){done(x)};x.onerror=function(){Z.say("Cannot reach the console.",true);done(null)};x.send(body)}
var tries=0,timer=0,lastDevs=[];
function show(id,on){Z.$(id).hidden=!on}
function render(d){
 show("sy",d.installed);if(!d.installed)return;
 var s=Z.$("sysum");s.textContent="";
 show("syoff",!d.running);show("syact",!!d.running);
 if(!d.running){s.appendChild(el("span",null,"Syncthing is off."));return}
 Z.$("symy").textContent=d.myid;
 lastDevs=d.devices;
 var L=Z.$("sydevs");L.textContent="";
 d.devices.forEach(function(v){var r=el("div","syrow");r.appendChild(el("strong",null,v.name||v.id));
  r.appendChild(el("span",null,v.connected?"connected":"not connected"));
  var b=el("button",null,"Remove");b.type="button";b.onclick=function(){
   if(!confirm("Remove this device?"))return;
   req("POST","/files/sync/device/remove",JSON.stringify({id:v.id}),function(x){if(!x||Z.jerr(x))return;Z.say("Device removed.");load()})};
  r.appendChild(b);L.appendChild(r)});
 var w=Z.$("sywith");w.textContent="";
 d.devices.forEach(function(v){var o=el("option",null,v.name||v.id);o.value=v.id;w.appendChild(o)});
 Z.$("syshare").disabled=!d.devices.length;
 var F=Z.$("syfolders");F.textContent="";
 var names={saves:"Saves",states:"Save states",screenshots:"Screenshots"};
 d.folders.forEach(function(f){var r=el("div","syrow");r.appendChild(el("strong",null,names[f.preset]||f.preset));
  var w2=f.devices.map(function(id){var n=id;lastDevs.forEach(function(v){if(v.id===id)n=v.name||id});return n}).join(", ");
  r.appendChild(el("span",null,w2||"Not shared with anyone."));
  var b=el("button",null,"Stop sharing");b.type="button";b.onclick=function(){
   req("POST","/files/sync/unshare",JSON.stringify({preset:f.preset}),function(x){if(!x||Z.jerr(x))return;Z.say("Stopped sharing.");load()})};
  r.appendChild(b);F.appendChild(r)})}
function load(){
 var n=Z.nextReq();
 req("GET","/files/sync",null,function(x){
  if(!Z.isReq(n)||!x||Z.jerr(x))return;
  var d={};try{d=JSON.parse(x.responseText)}catch(_){return}
  render(d);
  // after "Turn on" Syncthing needs a few seconds before it answers
  if(timer&&d.running){clearTimeout(timer);timer=0}
  else if(timer&&tries++<20){timer=setTimeout(load,1500)}
  else if(timer){timer=0;Z.say("Syncthing did not start. Try again in a moment.",true)}})}
function wait(){tries=0;if(timer)clearTimeout(timer);timer=setTimeout(load,1500)}
Z.$("syon").onclick=function(){Z.say("Starting...");
 req("POST","/files/sync/power",JSON.stringify({on:true}),function(x){if(!x||Z.jerr(x))return;wait()})};
Z.$("syoffbtn").onclick=function(){
 if(!confirm("Turn Syncthing off? Nothing is deleted."))return;
 req("POST","/files/sync/power",JSON.stringify({on:false}),function(x){if(!x||Z.jerr(x))return;Z.say("Syncthing is off.");load()})};
Z.$("sycopy").onclick=function(){var t=Z.$("symy").textContent;
 if(navigator.clipboard&&navigator.clipboard.writeText)navigator.clipboard.writeText(t).then(function(){Z.say("Copied.")},function(){Z.say("Select the code and copy it.",true)});
 else Z.say("Select the code and copy it.",true)};
Z.$("syadd").onclick=function(){
 var id=Z.$("sycode").value.trim();if(!id){Z.say("Type the code of the other device.",true);return}
 req("POST","/files/sync/device",JSON.stringify({id:id,name:Z.$("syname").value.trim()}),function(x){
  if(!x||Z.jerr(x))return;Z.say("Device added.");Z.$("sycode").value="";Z.$("syname").value="";load()})};
Z.$("syshare").onclick=function(){
 var dev=Z.$("sywith").value;if(!dev){Z.say("Add a device first.",true);return}
 req("POST","/files/sync/share",JSON.stringify({preset:Z.$("sywhat").value,device:dev}),function(x){if(!x||Z.jerr(x))return;Z.say("Folder shared.");load()})};
// the Cloud tab shows this part too: it loads with it
var prev=Z.ontab.cl;Z.ontab.cl=function(){if(prev)prev();load()};
)JS";

}  // namespace

ZF_REGISTER_WEB_MODULE([] {
  webtransfer::WebModule m;
  m.name = "sync";
  m.order = 71;
  m.owns = [](std::string_view p) {
    return p == "/files/sync" || p == "/files/sync/power" || p == "/files/sync/device" || p == "/files/sync/device/remove" || p == "/files/sync/share" ||
           p == "/files/sync/unshare";
  };
  m.route = websync::sync_route;
  m.css = R"CSS(#sy{margin-top:1.6rem;padding-top:1rem;border-top:1px solid var(--line)}#sy label{display:block;margin-top:.8rem}
#symy{display:block;margin:.4rem 0;padding:.5rem .6rem;border-radius:8px;background:var(--card2);word-break:break-all;font-size:.8rem}
#sy .syrow{display:flex;flex-wrap:wrap;gap:.4rem .8rem;align-items:center;margin:.5rem 0}#sy .syrow span{opacity:.8;flex:1;overflow-wrap:anywhere}#sy button{margin-top:.5rem})CSS";
  m.js = kSyncJs;
  return m;
});

}  // namespace zf
