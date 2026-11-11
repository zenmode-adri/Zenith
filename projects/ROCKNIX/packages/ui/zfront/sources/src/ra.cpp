#include "ra.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>

#include "json.hpp"
#include "menu_game.hpp"
#include "scraper_ss.hpp"
#include "settings.hpp"
#include "util.hpp"

namespace zf::ra {

namespace {

std::string replace_secret(std::string s, std::string_view secret) {
  if (secret.size() < 3) return s;  // a one or two letter password would mask half of any text
  for (size_t p = s.find(secret); p != std::string::npos; p = s.find(secret, p + 3)) s.replace(p, secret.size(), "***");
  return s;
}

bool write_all(int fd, const char* p, size_t n) {
  while (n) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= size_t(w);
  }
  return true;
}

}  // namespace

std::string login_config(std::string_view user, std::string_view pass) {
  namespace ss = zf::scraper::ss;
  std::string c;
  c += "url = \"https://retroachievements.org/dorequest.php\"\n";
  c += "data = \"r=login&u=" + ss::url_encode(user) + "&p=" + ss::url_encode(pass) + "\"\n";  // percent-encoded: no quote or backslash inside
  c += "silent\nconnect-timeout = 15\nmax-time = 30\nmax-filesize = 262144\nuser-agent = \"zfront\"\n";
  c += "write-out = \"\\n@@%{exitcode}|%{http_code}\\n\"\n";
  return c;
}

std::string clean_error(std::string_view s, std::string_view user, std::string_view pass) {
  std::string o = replace_secret(replace_secret(std::string(s), pass), user);
  for (char& c : o)
    if ((unsigned char)c < 0x20 || c == 0x7F) c = ' ';
  std::string_view t = trim(o);
  std::string r(t);
  if (r.size() > 120) {
    r.resize(120);
    while (!r.empty() && ((unsigned char)r.back() & 0xC0) == 0x80) r.pop_back();  // do not cut a UTF-8 sequence
    if (!r.empty() && (unsigned char)r.back() >= 0xC0) r.pop_back();
  }
  return r;
}

LoginResult parse_login(int curl_exit, int http, std::string_view body, std::string_view user, std::string_view pass) {
  LoginResult r;
  if (curl_exit != 0 || http == 0) {
    r.kind = LoginResult::Kind::Network;
    return r;
  }
  json::Value v;
  if (!json::parse(body, v) || !v.is_object() || !v.find("Success")) {
    r.kind = LoginResult::Kind::BadReply;
    return r;
  }
  if (v.boolean("Success")) {
    r.token = v.text("Token");
    if (r.token.empty()) {  // "Success" without a token is of no use to the emulators
      r.kind = LoginResult::Kind::BadReply;
      return r;
    }
    r.kind = LoginResult::Kind::Ok;
    return r;
  }
  r.kind = LoginResult::Kind::Rejected;
  r.error = clean_error(v.text("Error"), user, pass);
  return r;
}

std::string describe(const LoginResult& r) {
  switch (r.kind) {
    case LoginResult::Kind::Ok: return {};
    case LoginResult::Kind::NoCredentials: return tr("A valid account is required. Please register an account on https://retroachievements.org");
    case LoginResult::Kind::Network: return tr("NO NETWORK CONNECTION");
    case LoginResult::Kind::BadReply: return tr("UNEXPECTED ANSWER FROM RETROACHIEVEMENTS");
    case LoginResult::Kind::Rejected: return r.error.empty() ? std::string(tr("AN ERROR OCCURRED")) : r.error;
    case LoginResult::Kind::Dry: return {};
  }
  return {};
}

LoginResult login(Services& sv, const std::string& user, const std::string& pass) {
  LoginResult r;
  if (user.empty() || pass.empty()) return r;  // NoCredentials
  const char* zc = std::getenv("ZF_CURL");
  std::string curl = (zc && *zc && ::access(zc, X_OK) == 0) ? zc : "curl";
  const char* base = std::getenv("ZF_RA_TMP");
  std::string tmpl = std::string(base && *base ? base : "/tmp") + "/zf-ra-XXXXXX";
  std::vector<char> t(tmpl.begin(), tmpl.end());
  t.push_back('\0');
  if (!::mkdtemp(t.data())) {
    r.kind = LoginResult::Kind::Network;
    return r;
  }
  const std::string dir = t.data();
  const std::string cfg_path = dir + "/c";
  std::string cfg = login_config(user, pass);
  int fd = ::open(cfg_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  bool ok = fd >= 0 && write_all(fd, cfg.data(), cfg.size());
  if (fd >= 0) ::close(fd);
  std::fill(cfg.begin(), cfg.end(), '\0');  // no credentials left in freed memory
  if (!ok) {
    ::unlink(cfg_path.c_str());
    ::rmdir(dir.c_str());
    r.kind = LoginResult::Kind::Network;
    return r;
  }
  CmdResult c = sv.cmd.run(shell_quote(curl) + " -K " + shell_quote(cfg_path) + " 2>/dev/null", 45000);
  ::unlink(cfg_path.c_str());
  ::rmdir(dir.c_str());
  if (c.dry) {
    r.kind = LoginResult::Kind::Dry;
    return r;
  }
  size_t at = c.out.rfind("\n@@");
  int code = -1, http = 0;
  std::string body;
  if (at != std::string::npos) {
    body = c.out.substr(0, at);
    std::string_view tail(c.out);
    tail.remove_prefix(at + 3);
    size_t bar = tail.find('|');
    long a = 0, b = 0;
    if (bar != std::string_view::npos && parse_int(tail.substr(0, bar), a) && parse_int(trim(tail.substr(bar + 1)), b)) code = int(a), http = int(b);
  }
  r = parse_login(code, http, body, user, pass);
  std::fill(body.begin(), body.end(), '\0');
  std::fill(c.out.begin(), c.out.end(), '\0');
  return r;
}

Snapshot snapshot(const Ctx& cx) {
  Snapshot s;
  s.enabled = mg::conf_get(cx, key::kEnabled) == "1";
  s.user = mg::conf_get(cx, key::kUser);
  s.pass = mg::conf_get(cx, key::kPass);
  return s;
}

void apply_login(Ctx& cx, const Snapshot& before, std::function<void()> done) {
  auto finish = [done] {
    if (done) done();
  };
  const Snapshot now = snapshot(cx);
  const std::string token = mg::conf_get(cx, key::kToken);
  if (!now.enabled) {  // ES: switched off -> the token goes
    if (!token.empty()) mg::conf_put(cx, key::kToken, "");
    return finish();
  }
  if (before.enabled && before.user == now.user && before.pass == now.pass && !token.empty()) return finish();  // nothing changed
  if (cx.sv->cmd.dry_run) return finish();  // the harness never asks the network
  Ctx* c = &cx;
  std::shared_ptr<Services> sv = cx.sv;
  auto res = std::make_shared<LoginResult>();
  std::string user = now.user, pass = now.pass;
  sv->jobs.post([sv, res, user, pass] { *res = login(*sv, user, pass); },
                [c, res, user, pass, finish] {
                  if (res->kind == LoginResult::Kind::Dry) return finish();
                  if (res->ok()) {
                    mg::conf_put(*c, key::kToken, res->token);
                  } else {
                    mg::conf_put(*c, key::kToken, "");
                    mg::conf_put(*c, key::kEnabled, "0");
                    c->sv->toasts.push(std::string(tr("UNABLE TO ACTIVATE RETROACHIEVEMENTS:")) + " " + describe(*res), 6000);
                  }
                  finish();
                });
}

std::vector<std::string> sound_names() {
  std::vector<std::string> out;
  for (const char* d : {"/storage/roms/music/retroachievements", "/usr/share/libretro/sounds"}) {
    DIR* dir = ::opendir(mg::env().rooted(d).c_str());
    if (!dir) continue;
    while (const dirent* e = ::readdir(dir)) {
      std::string n = e->d_name;
      if (n.size() > 4 && ends_with(n, ".ogg")) {
        n.resize(n.size() - 4);
        if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
      }
    }
    ::closedir(dir);
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool in_main_menu(const Ctx& cx) {
  return mg::conf_get(cx, key::kEnabled) == "1" && !mg::conf_get(cx, key::kUser).empty() && cx.es.get_bool(key::kMenuItem, true);
}

bool has_stick_leds() {
  const char* v = std::getenv("DEVICE_ANALOG_STICKS_LED_CONTROL");
  return v && std::string(v) == "true";
}

}  // namespace zf::ra
