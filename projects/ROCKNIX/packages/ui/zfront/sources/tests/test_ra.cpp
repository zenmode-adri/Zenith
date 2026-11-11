// RetroAchievements: the JSON reader, the login request (what is sent, and that the credentials never travel in an argument) and how
// the answers are classified. No network: a fake `curl` (picked with $ZF_CURL) answers from files of a scratch directory.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "check.hpp"
#include "json.hpp"
#include "ra.hpp"

using namespace zf;
namespace fs = std::filesystem;

namespace {

std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
void put(const std::string& path, const std::string& text) {
  fs::create_directories(fs::path(path).parent_path());
  std::ofstream f(path, std::ios::binary);
  f << text;
}

}  // namespace

int main() {
  // ---- json
  {
    json::Value v;
    CHECK(json::parse(R"({"Success":true,"User":"Ana","Token":"abcé\n","Score":1234,"Nested":{"a":[1,2,{"b":null}]},"Str":"12"})", v));
    CHECK(v.boolean("Success"));
    CHECK_EQ(v.text("User"), std::string("Ana"));
    CHECK_EQ(v.text("Token"), std::string("abc\xC3\xA9\n"));
    CHECK_EQ(v.integer("Score"), 1234L);
    CHECK_EQ(v.integer("Str"), 12L);       // the RA API sends many numbers as strings
    CHECK_EQ(v.integer("Missing", 7), 7L);
    CHECK_EQ(v.text("Score"), std::string("1234"));
    const json::Value* n = v.find("Nested");
    CHECK(n && n->is_object());
    CHECK(n && n->find("a") && n->find("a")->is_array() && n->find("a")->arr.size() == 3);
    json::Value s;
    CHECK(json::parse(R"("🎮")", s));  // surrogate pair -> one 4-byte sequence
    CHECK_EQ(s.str, std::string("\xF0\x9F\x8E\xAE"));
    CHECK(!json::parse(R"("\ud83c")", s));        // lone surrogate
    CHECK(!json::parse(R"({"a":1} x)", s));       // trailing garbage
    CHECK(!json::parse(R"({"a":)", s));
    CHECK(!json::parse("", s));
    CHECK(!json::parse(R"({"a":"line
break"})", s));                                   // raw control character in a string
    // depth: 64 levels are fine, 65 are refused (no stack blowup on hostile input)
    CHECK(json::parse(std::string(60, '[') + std::string(60, ']'), s));
    CHECK(!json::parse(std::string(100000, '['), s));
  }

  // ---- the request: credentials percent-encoded inside a 0600-bound config, never in an argument
  {
    std::string c = ra::login_config("a&b=c", "p\"w\\ d\xC3\xA9");
    CHECK(c.find("data = \"r=login&u=a%26b%3Dc&p=p%22w%5C%20d%C3%A9\"") != std::string::npos);
    CHECK(c.find("https://retroachievements.org/dorequest.php") != std::string::npos);
    CHECK(c.find("p\"w") == std::string::npos);  // nothing raw
    CHECK(c.find("max-filesize") != std::string::npos);
  }

  // ---- answers
  {
    using K = ra::LoginResult::Kind;
    auto p = [](int ex, int http, std::string_view body) { return ra::parse_login(ex, http, body, "ana", "secretpw"); };
    ra::LoginResult r = p(0, 200, R"({"Success":true,"User":"ana","Token":"TOK123","Score":5})");
    CHECK(r.kind == K::Ok && r.token == "TOK123");
    CHECK(p(0, 200, R"({"Success":true,"User":"ana"})").kind == K::BadReply);  // no token: useless
    r = p(0, 401, R"({"Success":false,"Error":"Invalid User/Password combination. Please try again","Code":"invalid_credentials"})");
    CHECK(r.kind == K::Rejected);
    CHECK_EQ(r.error, std::string("Invalid User/Password combination. Please try again"));
    CHECK(p(6, 0, "").kind == K::Network);
    CHECK(p(0, 0, "").kind == K::Network);
    CHECK(p(0, 200, "<html>maintenance</html>").kind == K::BadReply);
    CHECK(p(0, 200, R"({"nope":1})").kind == K::BadReply);
    // a server text that echoes the secret, with control characters and too long
    r = p(0, 200, R"({"Success":false,"Error":"bad secretpw for ana\r\n\tend"})");
    CHECK(r.kind == K::Rejected && r.error.find("secretpw") == std::string::npos && r.error.find("ana") == std::string::npos);
    CHECK(r.error.find('\n') == std::string::npos && r.error.find('\t') == std::string::npos);
    std::string longmsg = R"({"Success":false,"Error":")" + std::string(500, 'x') + R"("})";
    CHECK(p(0, 200, longmsg).error.size() <= 120);
    CHECK_EQ(ra::describe(r).find("secretpw"), std::string::npos);
  }

  // ---- login() with the fake curl
  {
    char tmpl[] = "/tmp/zf-ra-test-XXXXXX";
    const char* d = ::mkdtemp(tmpl);
    CHECK(d != nullptr);
    if (!d) return 1;
    const std::string root = d, fake = root + "/fake", scratch = root + "/tmp";
    fs::create_directories(scratch);
    put(fake + "/curl", R"SH(#!/bin/bash
D="$ZF_FAKE_DIR"
echo "$*" >> "$D/argv.log"
[ "$1" = "-K" ] || exit 2
cp "$2" "$D/cfg.seen"
stat -c %a "$2" > "$D/cfg.mode"
if [ -f "$D/down" ]; then printf '\n@@6|000\n'; exit 6; fi
data=$(grep '^data' "$2")
case "$data" in
  *"p=GOODPASS"*) printf '{"Success":true,"User":"ana","Token":"TOKEN-OF-ANA"}\n@@0|200\n' ;;
  *"p=WEIRD"*) printf 'not json at all\n@@0|200\n' ;;
  *) printf '{"Success":false,"Error":"Invalid User/Password combination. Please try again"}\n@@0|401\n' ;;
esac
)SH");
    ::chmod((fake + "/curl").c_str(), 0755);
    ::setenv("ZF_CURL", (fake + "/curl").c_str(), 1);
    ::setenv("ZF_FAKE_DIR", fake.c_str(), 1);
    ::setenv("ZF_RA_TMP", scratch.c_str(), 1);

    Services sv;
    sv.cmd.dry_run = false;
    sv.cmd.keep_log = true;
    using K = ra::LoginResult::Kind;
    CHECK(ra::login(sv, "", "x").kind == K::NoCredentials);
    CHECK(ra::login(sv, "ana", "").kind == K::NoCredentials);
    CHECK(!fs::exists(fake + "/argv.log"));  // no process for an empty form
    ra::LoginResult r = ra::login(sv, "ana", "GOODPASS");
    CHECK(r.kind == K::Ok && r.token == "TOKEN-OF-ANA");
    CHECK_EQ(slurp(fake + "/cfg.mode"), std::string("600\n"));
    CHECK(slurp(fake + "/cfg.seen").find("p=GOODPASS") != std::string::npos);
    CHECK(slurp(fake + "/argv.log").find("GOODPASS") == std::string::npos);  // only the file name travels as an argument
    for (const std::string& line : sv.cmd.log) CHECK(line.find("GOODPASS") == std::string::npos && line.find("ana") == std::string::npos);
    CHECK(fs::is_empty(scratch));  // the config and its directory are gone
    CHECK(ra::login(sv, "ana", "nope").kind == K::Rejected);
    CHECK_EQ(ra::login(sv, "ana", "nope").error, std::string("Invalid User/Password combination. Please try again"));
    CHECK(ra::login(sv, "ana", "WEIRD").kind == K::BadReply);
    put(fake + "/down", "");
    CHECK(ra::login(sv, "ana", "GOODPASS").kind == K::Network);
    CHECK(fs::is_empty(scratch));
    sv.cmd.dry_run = true;  // the harness executor never asks
    CHECK(ra::login(sv, "ana", "GOODPASS").kind == K::Dry);

    for (const char* k : {"ZF_CURL", "ZF_FAKE_DIR", "ZF_RA_TMP"}) ::unsetenv(k);
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  return zt::done();
}
