// `clip_text` (the names of the rows of the default theme) cuts at the same place as the old one-by-one scan, UTF-8 included, and a very long
// name no longer costs a measure per character on every frame. Needs the Zenith theme folder (ZF_THEME_DIR) for its font; skipped without it.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "check.hpp"
#include "gfx.hpp"
#include "scene.hpp"
#include "theme.hpp"
#include "util.hpp"

using namespace zf;

namespace {

std::string theme_dir() {
  const char* e = std::getenv("ZF_THEME_DIR");
  return e ? e : "/usr/share/zfront/theme-dark";
}

// What `clip_text` was: one measure of the prefix per character, from the end.
size_t reference(Assets& a, std::string_view s, float px, float max) {
  std::vector<size_t> ends;
  for (size_t i = 0; i < s.size();) {
    utf8_next(s, i);
    ends.push_back(i);
  }
  size_t n = ends.size();
  auto width_of = [&](size_t count) { return std::ceil(a.measure(s.substr(0, count == 0 ? 0 : ends[count - 1]), px)) + 1.0f; };
  while (n > 0 && width_of(n) > max) --n;
  return n;
}

}  // namespace

int main() {
  Theme th;
  Assets a;
  std::string err;
  if (!Theme::load(theme_dir(), "1/1", th, err) || !a.load_font(th.path(th.str("font")), err)) {
    std::fprintf(stderr, "test_clip_text: theme not found (%s): skipped\n", err.c_str());
    return 0;
  }
  const std::vector<std::string> names = {"",
                                          "A",
                                          "Super Mario World",
                                          "The Legend of Zelda: Link's Awakening DX (USA, Europe) (Rev 1) (SGB Enhanced)",
                                          "Pok\xC3\xA9mon: Edici\xC3\xB3n Roja \xC3\x91" "and\xC3\xBA \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x82\xBF\xE3\x82\xA4\xE3\x83\x88\xE3\x83\xAB",
                                          std::string(300, 'W') + " " + std::string(200, 'i')};
  for (const std::string& s : names)
    for (float px : {12.0f, 16.0f, 24.0f})
      for (float max : {0.0f, 3.0f, 40.0f, 123.0f, 333.0f, 100000.0f}) {
        const size_t want = reference(a, s, px, max);
        const size_t got = clip_count(a, s, px, max);
        if (got != want) std::fprintf(stderr, "clip_count(\"%.20s\", %g, %g) = %zu, the scan gives %zu\n", s.c_str(), double(px), double(max), got, want);
        CHECK_EQ(got, want);
      }
  // the picture is the cut text, and a name that fits is whole
  const Color white = {255, 255, 255, 255};
  ImgId whole = clip_text(a, "Super Mario World", 16.0f, white, 100000.0f);
  CHECK_EQ(a.img(whole).w, a.img(a.text("Super Mario World", 16.0f, white)).w);
  ImgId cut = clip_text(a, "Super Mario World", 16.0f, white, 60.0f);
  CHECK(a.img(cut).w <= 61u);
  CHECK(a.img(cut).w > 0u);

  // a name of 100 000 characters: a measure per character from the end would read 5 000 000 000 of them
  std::string big;
  while (big.size() < 100000) big += "abc def ";
  const auto t0 = std::chrono::steady_clock::now();
  const size_t n = clip_count(a, big, 16.0f, 300.0f);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(n > 0 && n < big.size());
  CHECK(secs < 3.0);
  return zt::done();
}
