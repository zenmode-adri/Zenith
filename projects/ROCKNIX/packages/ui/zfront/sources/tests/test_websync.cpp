// websync: the Syncthing part of the Cloud tab. The real httpd server on 127.0.0.1 (port 0), a FAKE curl (a python script that plays
// Syncthing's REST interface from a small state file and logs what it gets) and a fake service command. Nothing touches /storage,
// nothing is started and no network is used.
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

#include "check.hpp"
#include "httpd.hpp"
#include "webcore.hpp"
#include "websync.hpp"
#include "webtransfer.hpp"

using namespace zf;
using std::string;

namespace {

const char* kPin = "4821";
const char* kMe8 = "AAAAAAA-BBBBBBB-CCCCCCC-DDDDDDD-EEEEEEE-FFFFFFF-GGGGGGG-HHHHHHH";
const char* kPc = "BBBBBBB-CCCCCCC-DDDDDDD-EEEEEEE-FFFFFFF-GGGGGGG-HHHHHHH-AAAAAAA";
const char* kPhone = "CCCCCCC-DDDDDDD-EEEEEEE-FFFFFFF-GGGGGGG-HHHHHHH-AAAAAAA-BBBBBBB";
const char* kKey = "KEY-abc_123";

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

// The fake curl: reads the settings curl would read from its standard input (url, request, headers, data), plays Syncthing from state.json
// and logs every call. <root>/mode: "down" answers like a refused connection; "refuse" answers 400 to every change.
const char* kFakeCurl = R"PY(#!/usr/bin/env python3
import sys, json, os, re
D = os.path.dirname(os.path.abspath(__file__))
open(D + "/argv.log", "a").write(" ".join(sys.argv[1:]) + "\n")
cfg = sys.stdin.read()
open(D + "/stdin.log", "a").write(cfg + "----\n")
def val(name):
    m = re.search(r'^' + name + r' = "((?:[^"\\]|\\.)*)"$', cfg, re.M)
    return re.sub(r'\\(.)', r'\1', m.group(1)) if m else ""
url, method, data = val("url"), val("request"), val("data")
key = re.search(r'X-API-Key: (\S+)"', cfg)
mode = open(D + "/mode").read().strip() if os.path.exists(D + "/mode") else ""
def out(code, body=""):
    sys.stdout.write(body + "\n" + str(code))
    sys.exit(0)
if mode == "down":
    sys.stderr.write("curl: (7) Failed to connect\n")
    sys.stdout.write("\n000")
    sys.exit(7)
if not key or key.group(1) != "KEY-abc_123":
    out(403, "forbidden")
path = url.split("8384", 1)[-1] if "8384" in url else url
sp = D + "/state.json"
st = json.load(open(sp)) if os.path.exists(sp) else {"devices": {"AAAAAAA-BBBBBBB-CCCCCCC-DDDDDDD-EEEEEEE-FFFFFFF-GGGGGGG-HHHHHHH": {"name": "console"}}, "folders": {}}
def save():
    json.dump(st, open(sp, "w"))
if method == "GET":
    if path == "/rest/system/status":
        out(200, json.dumps({"myID": "AAAAAAA-BBBBBBB-CCCCCCC-DDDDDDD-EEEEEEE-FFFFFFF-GGGGGGG-HHHHHHH"}))
    if path == "/rest/system/connections":
        up = open(D + "/up").read().split() if os.path.exists(D + "/up") else []
        out(200, json.dumps({"connections": {k: {"connected": k in up} for k in st["devices"]}}))
    if path == "/rest/config/devices":
        out(200, json.dumps([{"deviceID": k, "name": v["name"]} for k, v in st["devices"].items()]))
    if path == "/rest/config/folders":
        out(200, json.dumps(list(st["folders"].values())))
    m = re.match(r"/rest/config/folders/(.+)$", path)
    if m:
        f = st["folders"].get(m.group(1))
        out(200, json.dumps(f)) if f else out(404, "not found")
    out(404, "")
if mode == "refuse":
    out(400, "bad")
m = re.match(r"/rest/config/(devices|folders)/(.+)$", path)
if not m:
    out(404, "")
kind, ident = m.group(1), m.group(2)
if method == "PUT":
    obj = json.loads(data)
    if kind == "devices":
        st["devices"][ident] = {"name": obj["name"]}
    else:
        st["folders"][ident] = obj
    save()
    out(200, "")
if method == "DELETE":
    st[kind].pop(ident, None)
    if kind == "devices":  # Syncthing takes a removed device out of every folder too
        for f in st["folders"].values():
            f["devices"] = [d for d in f["devices"] if d["deviceID"] != ident]
    save()
    out(200, "")
out(405, "")
)PY";

const char* kFakeService = "#!/bin/sh\necho \"$1\" >> \"$(dirname \"$0\")/service.log\"\nexit 0\n";

struct Rig {
  string root, conf;
  http::Server srv;
  webtransfer::Env env;
  Rig() {
    char t[] = "/tmp/zf-websync-XXXXXX";
    root = ::mkdtemp(t);
    conf = root + "/config.xml";
    write_file(root + "/curl", kFakeCurl, 0755);
    write_file(root + "/service", kFakeService, 0755);
    write_file(root + "/syncthing", "#!/bin/sh\n", 0755);
    write_file(conf, string("<configuration><gui><apikey>") + kKey + "</apikey></gui></configuration>");
    ::setenv("ZF_CURL", (root + "/curl").c_str(), 1);
    ::setenv("ZF_SYNC_CONF", conf.c_str(), 1);
    ::setenv("ZF_SYNC_URL", "http://127.0.0.1:8384", 1);
    ::setenv("ZF_SYNC_ROOT", (root + "/storage").c_str(), 1);
    ::setenv("ZF_SYNCTHING", (root + "/syncthing").c_str(), 1);
    ::setenv("ZF_SYNC_SERVICE", (root + "/service").c_str(), 1);
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
    string cmd = "rm -rf '" + root + "'";
    if (std::system(cmd.c_str()) != 0) {
    }
  }
  uint16_t port() const { return srv.port(); }
  void mode(const char* m) { write_file(root + "/mode", m); }
  string state() { return slurp(root + "/state.json"); }
};

void pure() {
  using namespace websync;
  string o;
  CHECK(normalise_device_id(kPc, o) && o == kPc);
  CHECK(normalise_device_id("  bbbbbbb ccccccc-ddddddd-eeeeeee-fffffff-ggggggg-hhhhhhh-aaaaaaa ", o) && o == kPc);
  CHECK(normalise_device_id("BBBBBBBCCCCCCCDDDDDDDEEEEEEEFFFFFFFGGGGGGGHHHHHHHAAAAAAA", o) && o == kPc);
  CHECK(!normalise_device_id("", o) && !normalise_device_id("ABC", o));
  CHECK(!normalise_device_id("BBBBBBB-CCCCCCC-DDDDDDD-EEEEEEE-FFFFFFF-GGGGGGG-HHHHHHH-AAAAAA1", o));  // 1 is not in the alphabet
  CHECK(!normalise_device_id("BBBBBBB-CCCCCCC-DDDDDDD-EEEEEEE-FFFFFFF-GGGGGGG-HHHHHHH-AAAAAAA-B", o));
  CHECK(!normalise_device_id("BBBBBBB-CCCCCCC-DDDDDDD-EEEEEEE-FFFFFFF-GGGGGGG-HHHHHHH-AAAAAA\"", o));
  CHECK(api_key_from("<gui><apikey>abc-DEF_123</apikey></gui>") == "abc-DEF_123");
  CHECK(api_key_from("<gui></gui>").empty() && api_key_from("<apikey></apikey>").empty());
  CHECK(api_key_from("<apikey>a\"b</apikey>").empty() && api_key_from("<apikey>a\nb</apikey>").empty() && api_key_from("<apikey>a b</apikey>").empty());
  CHECK(find_preset("saves") && find_preset("states") && find_preset("screenshots") && !find_preset("") && !find_preset("../etc") && !find_preset("Saves"));
  CHECK(find_preset("screenshots")->send_only && !find_preset("saves")->send_only);
}

void flow() {
  Rig r;
  const uint16_t p = r.port();
  // not allowed without the PIN
  CHECK(status_of(get(p, "/files/sync", nullptr)) == 403);
  CHECK(status_of(post(p, "/files/sync/device", "{}", nullptr)) == 403);
  CHECK(status_of(post(p, "/files/sync/power", "{\"on\":true}", nullptr)) == 403);
  CHECK(!exists(r.root + "/service.log") && !exists(r.root + "/argv.log"));

  // the state: running, no other device, nothing shared
  string g = get(p, "/files/sync");
  CHECK(status_of(g) == 200 && has(body_of(g), "\"installed\":true") && has(body_of(g), "\"running\":true") && has(body_of(g), string("\"myid\":\"") + kMe8 + "\""));
  CHECK(has(body_of(g), "\"devices\":[]") && has(body_of(g), "\"folders\":[]"));
  CHECK(!has(body_of(g), kKey));  // the key never goes to the page

  // a device: lower case and spaces are tidied up; its own code, a wrong code and a name with a line break are refused
  CHECK(status_of(post(p, "/files/sync/device", string("{\"id\":\" bbbbbbb ccccccc-ddddddd-eeeeeee-fffffff-ggggggg-hhhhhhh-aaaaaaa\",\"name\":\"My PC\"}"))) == 200);
  CHECK(status_of(post(p, "/files/sync/device", string("{\"id\":\"") + kMe8 + "\",\"name\":\"me\"}")) == 400);
  CHECK(status_of(post(p, "/files/sync/device", "{\"id\":\"nope\",\"name\":\"x\"}")) == 400);
  CHECK(status_of(post(p, "/files/sync/device", string("{\"id\":\"") + kPhone + "\",\"name\":\"a\\nb\"}")) == 400);
  CHECK(status_of(post(p, "/files/sync/device", "not json")) == 400);
  CHECK(has(r.state(), kPc) && !has(r.state(), kPhone));
  write_file(r.root + "/up", kPc);
  g = get(p, "/files/sync");
  CHECK(has(body_of(g), string("{\"id\":\"") + kPc + "\",\"name\":\"My PC\",\"connected\":true}"));
  CHECK(!has(body_of(g), string("\"id\":\"") + kMe8));  // this console is not listed as "another device"

  // the API key goes through curl's standard input, never through its arguments
  CHECK(has(slurp(r.root + "/stdin.log"), string("X-API-Key: ") + kKey) && !has(slurp(r.root + "/argv.log"), kKey));
  CHECK(has(slurp(r.root + "/argv.log"), "--config -"));
  // the calls are served on the one thread of the web server: a Syncthing that does not answer holds the page for 3 s a call, not 10
  CHECK(has(slurp(r.root + "/argv.log"), "--max-time 3") && !has(slurp(r.root + "/argv.log"), "--max-time 10"));

  // sharing: only a device that is already added, only the three fixed folders, the path never comes from the page
  CHECK(status_of(post(p, "/files/sync/share", string("{\"preset\":\"saves\",\"device\":\"") + kPhone + "\"}")) == 400);
  CHECK(status_of(post(p, "/files/sync/share", string("{\"preset\":\"../etc\",\"device\":\"") + kPc + "\"}")) == 400);
  CHECK(status_of(post(p, "/files/sync/share", string("{\"preset\":\"\",\"device\":\"") + kPc + "\"}")) == 400);
  CHECK(!exists(r.root + "/storage"));
  CHECK(status_of(post(p, "/files/sync/share", string("{\"preset\":\"saves\",\"path\":\"/etc\",\"device\":\"") + kPc + "\"}")) == 200);
  CHECK(exists(r.root + "/storage/.config/retroarch/saves"));
  CHECK(has(r.state(), "\"path\": \"" + r.root + "/storage/.config/retroarch/saves\"") && has(r.state(), "sendreceive") && !has(r.state(), "/etc"));
  CHECK(status_of(post(p, "/files/sync/share", string("{\"preset\":\"screenshots\",\"device\":\"") + kPc + "\"}")) == 200);
  CHECK(has(r.state(), "sendonly") && exists(r.root + "/storage/roms/screenshots"));
  g = get(p, "/files/sync");
  CHECK(has(body_of(g), string("{\"preset\":\"saves\",\"label\":\"Zenith saves\",\"devices\":[\"") + kPc + "\"]}"));
  CHECK(has(body_of(g), "\"preset\":\"screenshots\""));

  // a second device joins a folder that is already shared: the first one stays
  CHECK(status_of(post(p, "/files/sync/device", string("{\"id\":\"") + kPhone + "\",\"name\":\"Phone\"}")) == 200);
  CHECK(status_of(post(p, "/files/sync/share", string("{\"preset\":\"saves\",\"device\":\"") + kPhone + "\"}")) == 200);
  g = get(p, "/files/sync");
  CHECK(has(body_of(g), string("\"devices\":[\"") + kPc + "\",\"" + kPhone + "\"]"));
  CHECK(has(r.state(), kMe8));  // and this console is in the folder's list

  // stop sharing, remove the device
  CHECK(status_of(post(p, "/files/sync/unshare", "{\"preset\":\"saves\"}")) == 200);
  CHECK(!has(body_of(get(p, "/files/sync")), "\"preset\":\"saves\""));
  CHECK(status_of(post(p, "/files/sync/unshare", "{\"preset\":\"nope\"}")) == 400);
  CHECK(status_of(post(p, "/files/sync/device/remove", string("{\"id\":\"") + kPc + "\"}")) == 200);
  CHECK(!has(body_of(get(p, "/files/sync")), kPc));
  CHECK(status_of(post(p, "/files/sync/device/remove", string("{\"id\":\"") + kMe8 + "\"}")) == 400);

  // Syncthing refuses a change: the page is told, not lied to
  r.mode("refuse");
  CHECK(status_of(post(p, "/files/sync/device", string("{\"id\":\"") + kPc + "\",\"name\":\"x\"}")) == 502);
  r.mode("");
  // wrong methods
  CHECK(status_of(post(p, "/files/sync", "")) == 405);
  CHECK(status_of(get(p, "/files/sync/device")) == 405);
}

void power_and_down() {
  Rig r;
  const uint16_t p = r.port();
  CHECK(status_of(post(p, "/files/sync/power", "{\"on\":true}")) == 200);
  CHECK(slurp(r.root + "/service.log") == "start\n");
  CHECK(status_of(post(p, "/files/sync/power", "{\"on\":false}")) == 200);
  CHECK(slurp(r.root + "/service.log") == "start\nstop\n");
  CHECK(status_of(post(p, "/files/sync/power", "nope")) == 400);
  // Syncthing does not answer (turned off, or still starting): the page gets "not running", and every change says so
  r.mode("down");
  string g = get(p, "/files/sync");
  CHECK(status_of(g) == 200 && has(body_of(g), "\"installed\":true") && has(body_of(g), "\"running\":false") && !has(body_of(g), "myid"));
  CHECK(status_of(post(p, "/files/sync/device", string("{\"id\":\"") + kPc + "\",\"name\":\"x\"}")) == 503);
  CHECK(status_of(post(p, "/files/sync/share", string("{\"preset\":\"saves\",\"device\":\"") + kPc + "\"}")) == 503);
  CHECK(status_of(post(p, "/files/sync/unshare", "{\"preset\":\"saves\"}")) == 503);
  CHECK(!exists(r.root + "/storage"));  // nothing was created
  r.mode("");
  // no API key yet (Syncthing has not written its configuration): not running
  write_file(r.conf, "<configuration></configuration>");
  CHECK(has(body_of(get(p, "/files/sync")), "\"running\":false"));
  // Syncthing is not on this console: the page hides the part, the switch says why it cannot work
  ::setenv("ZF_SYNCTHING", (r.root + "/missing").c_str(), 1);
  g = get(p, "/files/sync");
  CHECK(has(body_of(g), "\"installed\":false") && has(body_of(g), "\"running\":false"));
  CHECK(status_of(post(p, "/files/sync/power", "{\"on\":true}")) == 503);
}

void page() {
  Rig r;
  const string html = webtransfer::page_html();
  CHECK(has(html, "id=\"sy\"") && has(html, "id=\"sycode\"") && has(html, "id=\"syshare\"") && has(html, "id=\"t-cl\""));
  // the Syncthing part sits inside the Cloud tab's panel, after the cloud's own markup
  const size_t cl = html.find("<section id=\"p-cl\""), sy = html.find("id=\"sy\""), end = html.find("</section>", cl);
  CHECK(cl != string::npos && sy > cl && sy < end);
}

}  // namespace

int main() {
  pure();
  flow();
  power_and_down();
  page();
  std::printf("test_websync: ok\n");
  return zt::done();
}
