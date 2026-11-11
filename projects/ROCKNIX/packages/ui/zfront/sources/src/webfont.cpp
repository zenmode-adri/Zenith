// MODULE `webfont` (web module of webtransfer): the typeface of the Zenith themes for the page, so the phone sees the console's look.
//
//   GET /files/font.ttf      Rubik Regular from the installed Zenith theme (public: a typeface is no secret; 404 when the theme is not
//                            there, and the page falls back to the system font). Cached by the browser for a year.
//
// The file is read once (at most 1 MB) and kept in memory: the answer is made on the HTTP thread, never through the transfer
// worker, so it never waits for an upload. `ZF_WEBFONT` names another file (tests).
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "webcore.hpp"

namespace zf {
namespace webtransfer {
namespace {

constexpr size_t kMaxFont = 1u << 20;
const char* const kFonts[] = {
    "/usr/share/themes/es-theme-zenith-next/assets/fonts/Rubik-Regular.ttf",
    "/usr/share/themes/es-theme-zenith/assets/fonts/Rubik-Regular.ttf",
    "/storage/.config/emulationstation/themes/es-theme-zenith-next/assets/fonts/Rubik-Regular.ttf",
};

bool read_small(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  char b[16384];
  size_t n;
  while ((n = std::fread(b, 1, sizeof b, f)) > 0 && out.size() <= kMaxFont) out.append(b, n);
  std::fclose(f);
  return !out.empty() && out.size() <= kMaxFont;
}

/// The font bytes, empty when no theme has it. Read once; a missing font is looked for again at most once a minute.
const std::string& font() {
  static std::mutex m;
  static std::string data;
  static long long tried_at = -1;
  std::lock_guard<std::mutex> lk(m);
  if (!data.empty()) return data;
  const long long now = now_ms();
  if (tried_at >= 0 && now - tried_at < 60000) return data;
  tried_at = now;
  if (const char* e = std::getenv("ZF_WEBFONT")) {
    read_small(e, data);
    return data;
  }
  for (const char* p : kFonts)
    if (read_small(p, data)) break;
  return data;
}

http::Response font_route(const http::Request& req, const Env&) {
  if (req.method != "GET") return err(405, "method not allowed");
  const std::string& f = font();
  if (f.empty()) return err(404, "no font");
  http::Response r = reply(200, f, "font/ttf");
  r.headers.emplace_back("Cache-Control", "public, max-age=31536000, immutable");
  return r;
}

}  // namespace

ZF_REGISTER_WEB_MODULE([] {
  WebModule m;
  m.name = "font";
  m.order = 1;
  m.owns = [](std::string_view p) { return p == "/files/font.ttf"; };
  m.public_path = [](std::string_view p) { return p == "/files/font.ttf"; };
  m.route = font_route;
  return m;
});

}  // namespace webtransfer
}  // namespace zf
