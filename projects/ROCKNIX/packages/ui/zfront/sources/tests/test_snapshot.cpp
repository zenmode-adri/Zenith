#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "app.hpp"
#include "check.hpp"
#include "snapshot.hpp"

using namespace zf;

namespace {
// a root screen that allows the picture, and a menu that does not
struct Root : Screen {
  bool ok = true;
  Transition handle(Action a, Ctx&) override {
    if (a == Action::A) return Transition::push(std::make_unique<Menu>());
    return Transition::none();
  }
  struct Menu : Screen {
    Transition handle(Action a, Ctx&) override { return a == Action::B ? Transition::pop() : Transition::none(); }
    void draw(Ctx&, std::vector<Op>&) override {}
    bool is_menu() const override { return true; }
    bool snapshot_ok(const Ctx&) const override { return true; }  // even a screen that says yes is never alone on the stack
  };
  void draw(Ctx& cx, std::vector<Op>& out) override { out.push_back(Op::rect(0, 0, cx.w, cx.h, {10, 10, 10, 255})); }
  bool snapshot_ok(const Ctx&) const override { return ok; }
};
}  // namespace

static std::string slurp(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
static void spit(const std::string& p, const std::string& s) { std::ofstream(p, std::ios::binary | std::ios::trunc) << s; }

static snapshot::Image sample(uint32_t w, uint32_t h) {
  std::vector<uint8_t> rgba(size_t(w) * h * 4);
  for (size_t i = 0; i < rgba.size(); ++i) rgba[i] = uint8_t(i * 7);
  return snapshot::downscale_half(rgba.data(), w, h);
}

int main() {
  char tmpl[] = "/tmp/zf-snap-XXXXXX";
  const std::string root = mkdtemp(tmpl);
  const std::string d = root + "/cache";  // does not exist yet: save creates it

  snapshot::Key k;
  k.theme_set = "es-theme-zenith-dark";
  k.lang = "es_ES";
  k.build = "100-200";
  k.w = 720;
  k.h = 720;
  std::string why;

  // nothing saved: no snapshot
  CHECK(!snapshot::load(d, k, why));
  CHECK(!why.empty());

  // round trip
  const snapshot::Image im = sample(720, 720);
  CHECK_EQ(im.w, 360u);
  CHECK_EQ(im.h, 360u);
  std::string err;
  CHECK(snapshot::save(d, k, im, err));
  auto back = snapshot::load(d, k, why);
  CHECK(back.has_value());
  if (back) {
    CHECK_EQ(back->w, im.w);
    CHECK(back->px == im.px);
  }
  // atomic write: no temp file is left behind, only the picture
  {
    std::string ls = "ls " + d + " | wc -l > " + root + "/n.txt";
    CHECK_EQ(std::system(ls.c_str()), 0);
    CHECK_EQ(slurp(root + "/n.txt"), std::string("1\n"));
  }
  // a rewrite replaces the picture in one piece
  snapshot::Image im2 = im;
  im2.px[0] ^= 0xFFFF;
  CHECK(snapshot::save(d, k, im2, err));
  auto b2 = snapshot::load(d, k, why);
  CHECK(b2 && b2->px == im2.px);

  // any part of the key changing makes the picture stale
  snapshot::Key o = k;
  o.theme_set = "es-theme-iconic";
  CHECK(!snapshot::load(d, o, why));
  o = k;
  o.w = 640;
  o.h = 480;
  CHECK(!snapshot::load(d, o, why));
  o = k;
  o.lang = "en_US";
  CHECK(!snapshot::load(d, o, why));
  o = k;
  o.build = "101-200";
  CHECK(!snapshot::load(d, o, why));
  o = k;
  o.theme = "/some/theme";
  CHECK(!snapshot::load(d, o, why));
  CHECK(snapshot::load(d, k, why).has_value());  // and the right key still works

  // corrupt / truncated / foreign files are ignored
  const std::string file = snapshot::file_path(d);
  const std::string good = slurp(file);
  CHECK(!good.empty());
  std::string bad = good;
  bad[bad.size() / 2] ^= 0x55;  // a flipped pixel byte: checksum
  spit(file, bad);
  CHECK(!snapshot::load(d, k, why));
  spit(file, good.substr(0, good.size() - 100));  // cut short
  CHECK(!snapshot::load(d, k, why));
  spit(file, good + "x");  // longer than announced
  CHECK(!snapshot::load(d, k, why));
  spit(file, "");
  CHECK(!snapshot::load(d, k, why));
  spit(file, "not a snapshot at all");
  CHECK(!snapshot::load(d, k, why));
  spit(file, "ZFSNAP1\n" + k.text());  // header without newline
  CHECK(!snapshot::load(d, k, why));
  {  // announced dimensions beyond the bound with a matching key: refused before any allocation
    std::string h = "ZFSNAP1\n" + k.text() + "\n";
    const unsigned char big[8] = {0xFF, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF, 0xFF, 0x0F};
    h.append(reinterpret_cast<const char*>(big), 8);
    h.append(16, 'x');
    spit(file, h);
    CHECK(!snapshot::load(d, k, why));
  }
  spit(file, good);
  CHECK(snapshot::load(d, k, why).has_value());

  // bounded size: nothing is written for an oversized or empty picture
  snapshot::Image huge;
  huge.w = 2000;
  huge.h = 2000;
  huge.px.resize(size_t(huge.w) * huge.h);
  CHECK(!snapshot::save(d, k, huge, err));
  CHECK(!snapshot::save(d, k, snapshot::Image(), err));
  CHECK(snapshot::downscale_half(nullptr, 720, 720).px.empty());
  std::vector<uint8_t> px(4000 * 4000 * 4);
  CHECK(snapshot::downscale_half(px.data(), 4000, 4000).px.empty());  // 4 Mpx half > kMaxPixels

  // the colour of the average survives the packing (white stays white, black black)
  {
    std::vector<uint8_t> w(4 * 4 * 4, 255), z(4 * 4 * 4, 0);
    CHECK_EQ(snapshot::downscale_half(w.data(), 4, 4).px[0], uint16_t(0xFFFF));
    CHECK_EQ(snapshot::downscale_half(z.data(), 4, 4).px[0], uint16_t(0));
  }

  // key text is one line even with a hostile value
  o = k;
  o.theme = "a\nb";
  CHECK(o.text().find('\n') == std::string::npos);

  // the gate: only a carousel at rest, after the rest time, once per rest, with a minimum gap between pictures
  {
    snapshot::Gate g(3.0, 30.0);
    CHECK(!g.poll(true, 100.0));
    CHECK(!g.poll(true, 102.9));
    CHECK_EQ(*g.deadline(), 103.0);
    CHECK(g.poll(true, 103.0));
    CHECK(!g.poll(true, 110.0));  // already taken in this rest
    CHECK(!g.deadline().has_value());
    // a menu opens (not eligible): never fires, however long it stays open
    CHECK(!g.poll(false, 111.0));
    CHECK(!g.poll(false, 500.0));
    CHECK(!g.deadline().has_value());
    // back at rest, but the last picture is younger than the gap: waits for it
    CHECK(!g.poll(true, 120.0));
    CHECK(!g.poll(true, 130.0));
    CHECK_EQ(*g.deadline(), 133.0);
    CHECK(g.poll(true, 133.0));
    // interrupted rests restart the count
    snapshot::Gate g2(3.0, 0.0);
    CHECK(!g2.poll(true, 0.0));
    CHECK(!g2.poll(true, 2.0));
    CHECK(!g2.poll(false, 2.5));
    CHECK(!g2.poll(true, 3.0));
    CHECK(!g2.poll(true, 5.9));
    CHECK(g2.poll(true, 6.0));
  }

  // App: only the root screen, settled, without toasts; never with a menu open
  {
    Ctx cx;
    cx.w = cx.h = 720;
    App app(std::move(cx), std::make_unique<Root>(), make_status_source("/nonexistent"));
    app.render();
    CHECK(app.snapshot_eligible());
    app.handle(Action::A);  // a menu opens
    CHECK(!app.snapshot_eligible());
    app.handle(Action::B);
    for (int i = 0; i < 60; ++i) app.tick(1.0f / 60.0f);  // the leave motion is over
    CHECK(app.snapshot_eligible());
    app.cx.sv->toasts.push("Saved");
    CHECK(!app.snapshot_eligible());
  }

  // key of this build is stable inside a process
  CHECK_EQ(snapshot::build_id(), snapshot::build_id());

  std::string rm = "rm -rf " + root;
  (void)!std::system(rm.c_str());
  return zt::done();
}
