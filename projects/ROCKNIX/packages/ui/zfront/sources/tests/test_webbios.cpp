// webbios: the BIOS tab. The real httpd server on 127.0.0.1 (port 0) with the stream worker, a fake rocknix-systems table and a
// fake root in a temp dir, free space injected. Also parses the real copy of the script when the PC has it.
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "httpd.hpp"
#include "scraper_hash.hpp"
#include "webbios.hpp"
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

bool send_str(int fd, const string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
    if (n <= 0) return false;
    off += size_t(n);
  }
  return true;
}

string read_all(int fd, int ms = 4000) {
  string out;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
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
  return out;
}

int status_of(const string& r) {
  if (r.compare(0, 9, "HTTP/1.1 ") != 0 || r.size() < 12) return 0;
  return std::atoi(r.c_str() + 9);
}

string body_of(const string& resp) {
  size_t i = resp.find("\r\n\r\n");
  return i == string::npos ? string() : resp.substr(i + 4);
}

string talk(uint16_t port, const string& raw) {
  int fd = connect_to(port);
  if (fd < 0) return "";
  send_str(fd, raw);
  string r = read_all(fd);
  ::close(fd);
  return r;
}

string get(uint16_t port, const string& path, const char* pin = kPin) {
  string h = "GET " + path + " HTTP/1.1\r\nHost: x\r\n";
  if (pin) h += string("X-PIN: ") + pin + "\r\n";
  return talk(port, h + "\r\n");
}

/// POST /files/bios/put with `query` (already encoded).
string put(uint16_t port, const string& query, const string& body, const char* pin = kPin) {
  string h = "POST /files/bios/put" + query + " HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  if (pin) h += string("X-PIN: ") + pin + "\r\n";
  return talk(port, h + "\r\n" + body);
}

string slurp(const string& p) {
  string s;
  if (FILE* f = std::fopen(p.c_str(), "rb")) {
    char b[8192];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
  }
  return s;
}

void write_file(const string& p, const string& data) {
  if (FILE* f = std::fopen(p.c_str(), "wb")) {
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
  }
}

bool exists(const string& p) {
  struct stat sb;
  return ::lstat(p.c_str(), &sb) == 0;
}

std::vector<string> list(const string& dir) {
  std::vector<string> v;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (dirent* e = ::readdir(d))
      if (string(e->d_name) != "." && string(e->d_name) != "..") v.push_back(e->d_name);
    ::closedir(d);
  }
  std::sort(v.begin(), v.end());
  return v;
}

string pattern(size_t n, uint32_t seed = 12345) {
  string s(n, '\0');
  uint32_t x = seed;
  for (size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    s[i] = char(x >> 24);
  }
  return s;
}

string md5_of(const string& s) {
  scraper::Md5 h;
  h.update(s.data(), s.size());
  return h.hex();
}

bool has(const string& hay, const string& needle) { return hay.find(needle) != string::npos; }

// ---- the fake table -----------------------------------------------------------------------------------------------------

const string kGb = "game boy boot rom";
const string kAlpha = pattern(300000, 1);  // psx scph5501, variant one (bigger than a few reads)
const string kBeta = pattern(1000, 2);     // psx scph5501, variant two
const string kP1001 = pattern(5000, 3);
const string kAny = "whatever";

string fake_script() {
  return "#!/usr/bin/env python\nfrom hashlib import md5\n\nsystems = {\n"
         "    # ---- Nintendo ----\n"
         "    \"gb\": { \"name\": \"Game Boy\", \"biosFiles\": [ { \"md5\": \"" + md5_of(kGb) + "\", \"file\": \"bios/gb_bios.bin\"}, # boot\n"
         "                 #{ \"md5\": \"00000000000000000000000000000000\", \"file\": \"bios/commented.bin\"},\n"
         "                 { \"md5\": \"" + md5_of(kAny) + "\", \"file\": \"bios/gb_any.bin\", \"emulator\": \"libretro\", \"core\": \"gambatte\" } ] },\n"
         "    \"psx\": { \"name\": \"Sony PlayStation\", \"biosFiles\": [\n"
         "        { \"md5\": \"" + md5_of(kAlpha) + "\", \"file\": \"bios/scph5501.bin\"},\n"
         "        { \"md5\": \"" + md5_of(kBeta) + "\", \"file\": \"bios/scph5501.bin\"},\n"
         "        { \"md5\": \"" + md5_of(kP1001) + "\", \"file\": \"bios/scph1001.bin\"},\n"
         "        { \"md5\": \"" + md5_of(pattern(77, 9)) + "\", \"file\": \"bios/scph7502.bin\"} ] },\n"
         "    \"zipsys\": { \"name\": \"Zip System\", \"biosFiles\": [\n"
         "        { \"md5\": \"" + md5_of("in one") + "\", \"zippedFile\": \"one.rom\", \"file\": \"bios/pack.zip\"},\n"
         "        { \"md5\": \"" + md5_of("in two") + "\", \"altmd5\": \"" + md5_of("in 2") + "\", \"zippedFile\": \"two.rom\", \"file\": \"bios/pack.zip\"},\n"
         "        { \"md5\": \"" + md5_of("other") + "\", \"zippedFile\": \"x.rom\", \"file\": \"bios/other.zip\"} ] },\n"
         "    \"nogames\": { \"name\": \"No Games Here\", \"biosFiles\": [ { \"md5\": \"" + md5_of("deep") + "\", \"file\": \"bios/x/deep/y.bin\" },\n"
         "        { \"md5\": \"\", \"file\": \"bios/free.bin\" } ] },\n"
         "}\n\nclass BiosStatus:\n    MISSING = \"MISSING\"\n";
}

struct Rig {
  string root, bios, script;
  std::atomic<uint64_t> fake_free{~uint64_t(0)};
  http::Server srv;
  webtransfer::Env env;

  Rig() {
    char t[] = "/tmp/zf-webbios-XXXXXX";
    root = ::mkdtemp(t);
    bios = root + "/roms/bios";
    ::mkdir((root + "/roms").c_str(), 0755);
    script = root + "/rocknix-systems";
    write_file(script, fake_script());
    ::setenv("ZF_ROCKNIX_SYSTEMS", script.c_str(), 1);
    ::setenv("ZF_BIOS_ROOT", (root + "/roms").c_str(), 1);
    webtransfer::reset_for_test(kPin);
    auto sys = [](const char* name, const char* full, size_t count) {
      webtransfer::SystemInfo si;
      si.name = name;
      si.fullname = full;
      si.rom_dir = "/nonexistent";
      si.count = count;
      return si;
    };
    // psx is known by another short name: it is found by its full name, like library_doctor does
    webtransfer::publish_info({sys("gb", "Game Boy", 3), sys("playstation", "Sony PlayStation", 2), sys("zipsys", "", 1), sys("nogames", "No Games Here", 0)});
    env.free_space = [this](const string&, uint64_t& out) {
      out = fake_free.load();
      return true;
    };
    srv.set_stream_handler([this](const http::Request& r, http::BodyStream& b) { return webtransfer::stream_route(r, b, env); }, webtransfer::stream_pred);
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
  string at(const string& rel) const { return root + "/roms/" + rel; }
  void mk_bios() const { ::mkdir(bios.c_str(), 0755); }
};

/// The entry for `file` in a JSON answer: from its `{"file":"..."` to the closing brace.
string entry(const string& json, const string& file) {
  size_t a = json.find("{\"file\":\"" + file + "\"");
  if (a == string::npos) return "";
  return json.substr(a, json.find('}', a) - a + 1);
}

// ---- tests --------------------------------------------------------------------------------------------------------------

void test_md5_vectors() {
  struct V {
    const char* in;
    const char* want;
  };
  const V v[] = {{"", "d41d8cd98f00b204e9800998ecf8427e"},
                 {"a", "0cc175b9c0f1b6a831c399e269772661"},
                 {"abc", "900150983cd24fb0d6963f7d28e17f72"},
                 {"message digest", "f96b697d7cb7938d525a2f31aaf161d0"},
                 {"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"},
                 {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "d174ab98d277d9f5a5611c2c9f419d9f"},
                 {"12345678901234567890123456789012345678901234567890123456789012345678901234567890", "57edf4a22be3c955ac49da2e2107b67a"}};
  for (const V& t : v) CHECK_EQ(md5_of(t.in), string(t.want));
  scraper::Md5 h;  // a million 'a' in odd pieces
  string a(1000, 'a');
  for (int i = 0; i < 1000; ++i) h.update(a.data(), a.size());
  CHECK_EQ(h.hex(), string("7707d6ae4e027c70eea2a935c2296f21"));
}

void test_parse() {
  std::vector<webbios::BiosSystem> s = webbios::parse_systems(fake_script());
  CHECK_EQ(s.size(), size_t(4));
  CHECK_EQ(s[0].key, string("gb"));
  CHECK_EQ(s[0].name, string("Game Boy"));
  CHECK_EQ(s[0].files.size(), size_t(2));  // the commented entry is not there
  CHECK_EQ(s[0].files[0].file, string("bios/gb_bios.bin"));
  CHECK_EQ(s[0].files[0].md5s.size(), size_t(1));
  CHECK_EQ(s[1].files.size(), size_t(3));  // scph5501 twice = one file with two variants
  CHECK_EQ(s[1].entries, size_t(4));
  CHECK_EQ(s[1].files[0].md5s.size(), size_t(2));
  CHECK_EQ(s[1].files[0].md5s[1], md5_of(kBeta));
  CHECK_EQ(s[2].files.size(), size_t(2));
  CHECK(s[2].files[0].zipped && !s[1].files[0].zipped);
  CHECK_EQ(s[2].files[0].md5s.size(), size_t(3));  // two entries + altmd5
  CHECK_EQ(s[2].entries, size_t(3));
  CHECK(s[3].files[1].any && s[3].files[1].md5s.empty());
  CHECK(webbios::parse_systems("nothing here").empty());
  webbios::parse_systems("systems = {\n \"a\": { \"name\": \"A\", \"biosFiles\": [ { \"md5\": \"x\", ");  // a cut table must not crash
  CHECK(webbios::parse_systems("systems = { \"a\" }").empty());
  CHECK(webbios::parse_systems("systems = {").empty());
  // upper-case md5 is read as lower case
  auto up = webbios::parse_systems("systems = { \"a\": { \"name\": \"A\", \"biosFiles\": [ { \"md5\": \"ABCDEF\", \"file\": \"bios/a\" } ] } }");
  CHECK_EQ(up.size(), size_t(1));
  CHECK_EQ(up[0].files[0].md5s[0], string("abcdef"));
}

void test_status_cache_and_order() {
  Rig r;
  webtransfer::set_enabled(true);
  r.mk_bios();
  write_file(r.at("bios/gb_bios.bin"), kGb);          // ok
  write_file(r.at("bios/scph5501.bin"), kBeta);       // ok: the second variant
  write_file(r.at("bios/scph1001.bin"), "not it");    // wrong
  write_file(r.at("bios/pack.zip"), "PK pretend");    // unchecked
  write_file(r.at("bios/gb_any.bin"), kAny);          // ok
  CHECK_EQ(status_of(get(r.port(), "/files/bios", nullptr)), 403);
  CHECK_EQ(status_of(get(r.port(), "/files/bios", "0000")), 403);
  const size_t h0 = webbios::hash_count();
  string a = get(r.port(), "/files/bios");
  CHECK_EQ(status_of(a), 200);
  string j = body_of(a);
  CHECK(has(j, "\"all\":false"));
  CHECK(has(entry(j, "bios/gb_bios.bin"), "\"status\":\"ok\""));
  CHECK(has(entry(j, "bios/scph5501.bin"), "\"status\":\"ok\""));
  CHECK(has(entry(j, "bios/scph5501.bin"), md5_of(kAlpha)) && has(entry(j, "bios/scph5501.bin"), md5_of(kBeta)));
  string w = entry(j, "bios/scph1001.bin");
  CHECK(has(w, "\"status\":\"wrong\"") && has(w, "\"have\":\"" + md5_of("not it") + "\""));
  CHECK(has(entry(j, "bios/scph7502.bin"), "\"status\":\"missing\""));
  CHECK(has(entry(j, "bios/pack.zip"), "\"status\":\"unchecked\"") && has(entry(j, "bios/pack.zip"), "\"zipped\":true"));
  CHECK(has(entry(j, "bios/other.zip"), "\"status\":\"missing\""));
  CHECK(has(j, "\"missing\":2") && has(j, "\"wrong\":1") && has(j, "\"unchecked\":1") && has(j, "\"ok\":3"));
  CHECK(!has(j, "nogames") && !has(j, "No Games Here") && !has(j, "x/deep"));  // no games: not listed by default
  CHECK(has(j, "\"name\":\"Sony PlayStation\""));                                // found by its full name
  // systems with problems first: gb has none, psx and zipsys do
  CHECK(j.find("\"key\":\"gb\"") > j.find("\"key\":\"psx\""));
  CHECK(j.find("\"key\":\"gb\"") > j.find("\"key\":\"zipsys\""));
  CHECK_EQ(webbios::hash_count() - h0, size_t(4));  // gb_bios, scph5501, scph1001, gb_any: the zip is never read
  string b = get(r.port(), "/files/bios?all=1");
  CHECK(has(b, "\"all\":true") && has(b, "x/deep/y.bin") && has(entry(body_of(b), "bios/free.bin"), "\"status\":\"missing\""));
  CHECK_EQ(webbios::hash_count() - h0, size_t(4));  // nothing hashed twice
  write_file(r.at("bios/scph1001.bin"), "not it, longer");  // changed: hashed again, only that one
  get(r.port(), "/files/bios?all=1");
  CHECK_EQ(webbios::hash_count() - h0, size_t(5));
  // a folder or a symlink in place of a file is missing
  ::mkdir(r.at("bios/scph7502.bin").c_str(), 0755);
  CHECK(has(entry(body_of(get(r.port(), "/files/bios")), "bios/scph7502.bin"), "\"status\":\"missing\""));
  // the switch
  webtransfer::set_enabled(false);
  CHECK_EQ(status_of(get(r.port(), "/files/bios")), 404);
  CHECK_EQ(status_of(put(r.port(), "?file=bios/gb_bios.bin", "x")), 404);
}

void test_upload() {
  Rig r;
  webtransfer::set_enabled(true);
  const size_t h0 = webbios::hash_count();
  // right file, folder created
  CHECK(!exists(r.bios));
  string a = put(r.port(), "?file=bios/scph5501.bin", kAlpha);
  CHECK_EQ(status_of(a), 200);
  CHECK(has(body_of(a), "\"status\":\"ok\"") && has(body_of(a), md5_of(kAlpha)));
  CHECK(slurp(r.at("bios/scph5501.bin")) == kAlpha);
  CHECK(list(r.bios) == std::vector<string>{"scph5501.bin"});  // no .part left
  // the next list does not read it again
  CHECK(has(entry(body_of(get(r.port(), "/files/bios")), "bios/scph5501.bin"), "\"status\":\"ok\""));
  CHECK_EQ(webbios::hash_count() - h0, size_t(0));
  // there already and right: 409 unless replace=1; the other variant replaces it
  CHECK_EQ(status_of(put(r.port(), "?file=bios/scph5501.bin", kBeta)), 409);
  CHECK(slurp(r.at("bios/scph5501.bin")) == kAlpha);
  CHECK_EQ(status_of(put(r.port(), "?file=bios/scph5501.bin&replace=1", kBeta)), 200);
  CHECK(slurp(r.at("bios/scph5501.bin")) == kBeta);
  // wrong file: 422 with its md5, nothing left; the old (wrong) file is untouched
  write_file(r.at("bios/scph1001.bin"), "old wrong");
  string bad = put(r.port(), "?file=bios/scph1001.bin", "this is not it");
  CHECK_EQ(status_of(bad), 422);
  CHECK(has(body_of(bad), "\"error\":\"not the expected file\"") && has(body_of(bad), "\"md5\":\"" + md5_of("this is not it") + "\""));
  CHECK(slurp(r.at("bios/scph1001.bin")) == "old wrong");
  CHECK(list(r.bios) == (std::vector<string>{"scph1001.bin", "scph5501.bin"}));
  // an existing wrong file is simply replaced by the right one, no replace=1 needed
  CHECK_EQ(status_of(put(r.port(), "?file=bios/scph1001.bin", kP1001)), 200);
  CHECK(slurp(r.at("bios/scph1001.bin")) == kP1001);
  // nested folders: a refused upload leaves no folders behind
  CHECK_EQ(status_of(put(r.port(), "?file=bios/x/deep/y.bin", "nope")), 422);
  CHECK(!exists(r.at("bios/x")));
  CHECK_EQ(list(r.bios).size(), size_t(2));
  // force keeps it (the page warned); the list then says wrong
  string f = put(r.port(), "?file=bios/x/deep/y.bin&force=1", "nope");
  CHECK_EQ(status_of(f), 200);
  CHECK(has(body_of(f), "\"status\":\"wrong\""));
  CHECK(slurp(r.at("bios/x/deep/y.bin")) == "nope");
  CHECK(has(entry(body_of(get(r.port(), "/files/bios?all=1")), "bios/x/deep/y.bin"), "\"status\":\"wrong\""));
  // md5 "" accepts anything; a zip cannot be checked: accepted, `unchecked`, and not replaced without replace=1
  CHECK(has(body_of(put(r.port(), "?file=bios/free.bin", "anything")), "\"status\":\"ok\""));
  string z = put(r.port(), "?file=bios/pack.zip", "PK whatever");
  CHECK_EQ(status_of(z), 200);
  CHECK(has(body_of(z), "\"status\":\"unchecked\""));
  CHECK_EQ(status_of(put(r.port(), "?file=bios/pack.zip", "PK again")), 409);
  CHECK(slurp(r.at("bios/pack.zip")) == "PK whatever");
  CHECK_EQ(status_of(put(r.port(), "?file=bios/pack.zip&replace=1", "PK again")), 200);
  CHECK(slurp(r.at("bios/pack.zip")) == "PK again");
  // other refusals
  CHECK_EQ(status_of(put(r.port(), "?file=bios/free.bin", "")), 400);  // empty
  CHECK_EQ(status_of(put(r.port(), "", "x")), 400);
  CHECK_EQ(status_of(put(r.port(), "?file=bios/gb_bios.bin", "x", nullptr)), 403);
  CHECK_EQ(status_of(put(r.port(), "?file=bios/gb_bios.bin", "x", "0000")), 403);
  CHECK(!exists(r.at("bios/gb_bios.bin")));
  // a leftover .part of an earlier upload is replaced
  write_file(r.at("bios/gb_bios.bin.part"), "stale");
  CHECK_EQ(status_of(put(r.port(), "?file=bios/gb_bios.bin", kGb)), 200);
  CHECK(!exists(r.at("bios/gb_bios.bin.part")));
  // free space
  r.fake_free = 5;
  CHECK_EQ(status_of(put(r.port(), "?file=bios/gb_any.bin", kAny)), 413);
  CHECK(!exists(r.at("bios/gb_any.bin")) && !exists(r.at("bios/gb_any.bin.part")));
  r.fake_free = ~uint64_t(0);
  CHECK_EQ(status_of(put(r.port(), "?file=bios/gb_any.bin", kAny)), 200);
  CHECK(!exists(r.at("bios/gb_any.bin.part")));
}

void test_paths_not_in_the_list() {
  Rig r;
  webtransfer::set_enabled(true);
  r.mk_bios();
  write_file(r.root + "/outside.bin", "keep");
  for (const char* q : {"?file=bios/other.bin", "?file=../outside.bin", "?file=bios/../../outside.bin", "?file=%2E%2E/outside.bin", "?file=/etc/passwd",
                        "?file=%2Fetc%2Fpasswd", "?file=bios/../bios/gb_bios.bin", "?file=bios//gb_bios.bin", "?file=bios/gb_bios.bin/", "?file=./bios/gb_bios.bin",
                        "?file=BIOS/gb_bios.bin", "?file=bios/gb_bios.bin.part", "?file=bios%2Fgb_bios.bin%00", "?file=", "?file=bios/x/deep"}) {
    const int s = status_of(put(r.port(), q, "payload"));
    if (s != 400) std::fprintf(stderr, "path test: %s gave %d\n", q, s);
    CHECK_EQ(s, 400);
  }
  CHECK(slurp(r.root + "/outside.bin") == "keep");
  CHECK(list(r.bios).empty());
  CHECK(list(r.root + "/roms") == std::vector<string>{"bios"});
}

void test_symlinks() {
  Rig r;
  webtransfer::set_enabled(true);
  const string out = r.root + "/outside";
  ::mkdir(out.c_str(), 0755);
  write_file(out + "/gb_bios.bin", kGb);
  CHECK_EQ(::symlink(out.c_str(), r.bios.c_str()), 0);  // roms/bios -> outside
  string j = body_of(get(r.port(), "/files/bios"));
  CHECK(has(entry(j, "bios/gb_bios.bin"), "\"status\":\"missing\""));  // not followed: the file there does not count
  CHECK_EQ(status_of(put(r.port(), "?file=bios/scph1001.bin", kP1001)), 400);
  CHECK_EQ(status_of(put(r.port(), "?file=bios/gb_bios.bin&replace=1", kGb)), 400);
  CHECK_EQ(status_of(put(r.port(), "?file=bios/x/deep/y.bin&force=1", "nope")), 400);
  CHECK(list(out) == std::vector<string>{"gb_bios.bin"});
  CHECK(slurp(out + "/gb_bios.bin") == kGb);
  // a symlinked file inside a real folder: not a file of ours either
  ::unlink(r.bios.c_str());
  r.mk_bios();
  CHECK_EQ(::symlink((out + "/gb_bios.bin").c_str(), r.at("bios/gb_bios.bin").c_str()), 0);
  CHECK(has(entry(body_of(get(r.port(), "/files/bios")), "bios/gb_bios.bin"), "\"status\":\"missing\""));
  CHECK_EQ(status_of(put(r.port(), "?file=bios/gb_bios.bin&replace=1", kGb)), 400);
  CHECK(slurp(out + "/gb_bios.bin") == kGb);
  // a symlink standing in a folder component of a new path
  ::mkdir(r.at("bios/x").c_str(), 0755);
  ::rmdir(r.at("bios/x").c_str());
  CHECK_EQ(::symlink(out.c_str(), r.at("bios/x").c_str()), 0);
  CHECK_EQ(status_of(put(r.port(), "?file=bios/x/deep/y.bin&force=1", "nope")), 400);
  CHECK(list(out) == std::vector<string>{"gb_bios.bin"});
  // and a dangling .part symlink is not written through
  ::unlink(r.at("bios/x").c_str());
  CHECK_EQ(::symlink((out + "/stolen").c_str(), r.at("bios/scph1001.bin.part").c_str()), 0);
  CHECK_EQ(status_of(put(r.port(), "?file=bios/scph1001.bin", kP1001)), 200);  // the stale link is removed first, then a plain file is made
  CHECK(!exists(out + "/stolen"));
}

void test_page() {
  string html = webtransfer::page_html();
  CHECK(has(html, "<button type=\"button\" id=\"t-bi\">BIOS</button>"));
  CHECK(has(html, "<section id=\"p-bi\" hidden>"));
  CHECK(has(html, "Show all systems") && has(html, "This is not the expected file. Save it anyway?") && has(html, "/files/bios/put"));
  CHECK(html.find("id=\"t-bi\"") > html.find("id=\"t-sh\""));
  for (const char* bad : {"https://", "src=", "href=", "@import", "<link", "<iframe", "//cdn", "<img src", "window.open", "eval(", "DELETE"})
    CHECK(!has(html, bad));
  if (const char* d = std::getenv("ZF_PAGE_DUMP")) write_file(d, html);
}

void test_real_script() {
  const string path = "/mnt/c/tmp/zt/x10/rocknix-systems";
  string text = slurp(path);
  if (text.empty()) return;  // not on this machine
  std::vector<webbios::BiosSystem> s = webbios::parse_systems(text);
  size_t entries = 0, files = 0, zipped = 0, bad_names = 0;
  for (const auto& b : s) {
    entries += b.entries;
    files += b.files.size();
    for (const auto& f : b.files) {
      zipped += f.zipped;
      size_t from = 0;
      for (;;) {
        size_t e = f.file.find('/', from);
        if (!webtransfer::valid_name(f.file.substr(from, e == string::npos ? string::npos : e - from))) ++bad_names;
        if (e == string::npos) break;
        from = e + 1;
      }
    }
  }
  CHECK_EQ(s.size(), size_t(86));
  CHECK_EQ(entries, size_t(566));  // counted with Python's ast on the same file
  CHECK_EQ(files, size_t(299));
  CHECK_EQ(bad_names, size_t(0));
  CHECK(zipped > 0);
  const webbios::BiosSystem* psx = nullptr;
  for (const auto& b : s)
    if (b.key == "psx") psx = &b;
  CHECK(psx != nullptr);
  if (psx) {
    CHECK_EQ(psx->name, string("PSX"));
    bool found = false;
    for (const auto& f : psx->files)
      if (f.file == "bios/scph5501.bin") {
        found = true;
        CHECK_EQ(f.md5s.size(), size_t(1));
        CHECK_EQ(f.md5s[0], string("490f666e1afb15b7362b406ed1cea246"));
      }
    CHECK(found);
  }
  for (const auto& b : s)
    if (b.key == "dreamcast") {
      CHECK_EQ(b.files[0].file, string("bios/dc_boot.bin"));
      CHECK_EQ(b.files[0].md5s[0], string("e10c53c2f8b90bab96ead2d368858623"));
    }
}

}  // namespace

int main() {
  test_md5_vectors();
  test_parse();
  test_status_cache_and_order();
  test_upload();
  test_paths_not_in_the_list();
  test_symlinks();
  test_page();
  test_real_script();
  return zt::done();
}
