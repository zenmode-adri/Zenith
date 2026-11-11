// The console side of the web transfer's QR code and paired phones: the rows of the TRANSFER FILES menu under NETWORK (SHOW QR CODE, PAIRED
// DEVICES), the screen `menu.webqr` (a QR code black on white whatever the theme, with the address and the PIN under it) and the list
// `menu.webdevices` (each paired phone: select it to forget it, or forget them all). The QR text is `http://<ip>:<port>/files#pin=<PIN>`:
// the part after '#' never reaches the server, and the page removes it from the address bar at once.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app.hpp"
#include "i18n.hpp"
#include "menu.hpp"
#include "menu_network.hpp"
#include "qr.hpp"
#include "registry.hpp"
#include "remake.hpp"
#include "scene.hpp"
#include "services.hpp"
#include "ui_style.hpp"
#include "webcore.hpp"
#include "webpair.hpp"
#include "webtransfer.hpp"

namespace zf {
namespace {

using remake::Grid;

constexpr int kQuiet = 4;  // light modules around the code, as the standard asks

std::string sc(const char* key) { return i18n::sentence_case(tr(key)); }

/// One line of text centred on `cx`, its middle on `cy`; returns its height.
float centred(Assets& a, std::vector<Op>& ops, const std::string& s, float cx, float cy, int px, Color c, float max_w) {
  if (s.empty()) return 0;
  const ImgId id = a.text_ellipsis(s, float(px), c, max_w);
  const float tw = float(a.img(id).w), th = float(a.img(id).h);
  const float x = std::round(cx - tw / 2.0f), y = std::round(cy - th / 2.0f);
  ops.push_back(Op::image(id, x, y, tw, th));
  if (remake::audit_sink()) remake::audit(x, y, tw, th, "\"" + s.substr(0, 18) + "\"", 'C');
  return th;
}

class QrScreen : public Screen {
 public:
  explicit QrScreen(Ctx&) { load(); }
  bool is_menu() const override { return true; }
  /// The address can change under an open screen (the Wi-Fi joins or drops, a lease changes): looked at every two seconds, the code follows.
  bool tick(float dt, Ctx&) override {
    left_ -= dt;
    if (left_ > 0) return false;
    left_ = kLivePoll;
    return load();
  }
  std::optional<float> wake_in() const override { return left_ + 0.002f; }
  Transition handle(Action a, Ctx&) override {
    if (a == Action::B) return Transition::pop();
    if (a == Action::Start) return Transition::close_menus();
    return Transition::none();
  }
  void draw(Ctx& cx, std::vector<Op>& ops) override {
    const Grid g(cx.w, cx.h);
    const ui::Style st = ui::Style::from(cx.th);
    Assets& a = cx.assets;
    ops.push_back(Op::rect(0, 0, g.w, g.h, st.bg));
    remake::menu_backdrop(cx.th, a, g.w, g.h, ops);
    const size_t stack_from = ops.size();
    status_bar(cx.th, a, g.w, g.h, cx.status, ops);
    const ui::Metrics mt = ui::Metrics::from(cx.th, g.w, g.h);
    {  // the title, on the line every menu uses
      const std::string title = sc("QR CODE");
      const float tmax = ui::title_max_w(st, mt, ops, stack_from, g.w * 0.55f);
      int tpx = mt.title_px;  // never taller than the status bar (4 cells): a tall font would stick out over the top margin
      ImgId id = a.text_ellipsis(title, float(tpx), st.txt, tmax);
      while (float(a.img(id).h) > float(g.bar_h()) && tpx > 8) id = a.text_ellipsis(title, float(--tpx), st.txt, tmax);
      const float tw = float(a.img(id).w), th = float(a.img(id).h);
      ops.push_back(Op::image(id, mt.title_left, std::round(mt.header_cy - th / 2.0f), tw, th));
      ui::header_box() = {true, mt.title_left, mt.title_left + tw, mt.header_cy - th / 2.0f, mt.header_cy + th / 2.0f, float(g.content_top())};
    }
    // above the help: the help row of the views, or a theme's own pill (Zenith Minimal), which is taller
    const float help_top = std::min(float(g.help_top()), mt.help_bottom - mt.help_h - float(g.U(2)));
    const float top = float(g.content_top()), bottom = help_top, mid = g.w / 2.0f, wide = g.w - 2.0f * float(g.margin());
    if (code_.version == 0) {
      centred(a, ops, sc("NOT CONNECTED"), mid, (top + bottom) / 2.0f, g.F(18, 13), st.dim, wide);
    } else {
      draw_code(a, ops, st, g, top, bottom, mid, wide);
    }
    help_bar(cx.th, a, g.w, g.h, help_prompts(), ops);
  }

 private:
  static constexpr float kLivePoll = 2.0f;
  /// Takes the address and the PIN as they are now; true when the code changed.
  bool load() {
    const std::vector<std::string> ips = netmenu::local_ip_list();
    const std::string address = ips.empty() ? std::string() : netmenu::web_address(ips[0]);
    const std::string pin = ips.empty() ? std::string() : webtransfer::pin();
    if (address == address_ && pin == pin_) return false;
    address_ = address;
    pin_ = pin;
    code_ = qr::Code();
    if (!address_.empty() && !qr::encode(address_ + "#pin=" + pin_, qr::Ecc::M, code_)) code_ = qr::Code();
    return true;
  }
  void draw_code(Assets& a, std::vector<Op>& ops, const ui::Style& st, const Grid& g, float top, float bottom, float mid, float wide) {
    const int px1 = g.F(15, 12), px2 = g.F(22, 15);
    const std::string pin_line = std::string(tr("PIN")) + " " + pin_;
    const float gap = float(g.P(8));
    // the heights of the two lines, from images nothing draws, so the code gets what is left
    const float h1 = float(a.img(a.text_ellipsis(address_, float(px1), st.txt, wide)).h);
    const float h2 = float(a.img(a.text_ellipsis(pin_line, float(px2), st.txt, wide)).h);
    const float room = (bottom - top) - h1 - h2 - 2.0f * gap;
    const int modules = code_.size + 2 * kQuiet;
    const int cell = std::max(1, int(std::min(room, wide) / float(modules)));  // whole pixels: sharp edges for a camera
    const float side = float(cell * modules);
    const float total = side + gap + h1 + gap / 2.0f + h2;
    const float y0 = std::round(top + std::max(0.0f, ((bottom - top) - total) / 2.0f));
    const float x0 = std::round(mid - side / 2.0f);
    ops.push_back(Op::rect(x0, y0, side, side, Color{255, 255, 255, 255}));
    if (remake::audit_sink()) remake::audit(x0, y0, side, side, "qr", 'C');
    const Color black{0, 0, 0, 255};
    for (int y = 0; y < code_.size; ++y) {
      for (int x = 0; x < code_.size; ++x) {
        if (!code_.at(x, y)) continue;
        int e = x;
        while (e + 1 < code_.size && code_.at(e + 1, y)) ++e;  // one rectangle per run of dark modules
        ops.push_back(Op::rect(x0 + float((x + kQuiet) * cell), y0 + float((y + kQuiet) * cell), float((e - x + 1) * cell), float(cell), black));
        x = e;
      }
    }
    float y = y0 + side + gap;
    centred(a, ops, address_, mid, y + h1 / 2.0f, px1, st.dim, wide);
    y += h1 + gap / 2.0f;
    centred(a, ops, pin_line, mid, y + h2 / 2.0f, px2, st.txt, wide);
  }
  static const HelpPrompts& help_prompts() {
    static HelpPrompts p;
    static std::string b;
    b = i18n::sentence_case(tr("BACK"));
    p = {{"B", b.c_str()}};
    return p;
  }
  std::string address_, pin_;
  qr::Code code_;
  float left_ = kLivePoll;
};

// "5 minutes ago", "just now"... for the list of devices
std::string ago(long long seconds) {
  if (seconds < 60) return tr("JUST NOW");
  char b[48];
  if (seconds < 3600) std::snprintf(b, sizeof b, i18n::ntr("%d MINUTE AGO", "%d MINUTES AGO", (unsigned long)(seconds / 60)), int(seconds / 60));
  else if (seconds < 86400) std::snprintf(b, sizeof b, i18n::ntr("%d HOUR AGO", "%d HOURS AGO", (unsigned long)(seconds / 3600)), int(seconds / 3600));
  else std::snprintf(b, sizeof b, i18n::ntr("%d DAY AGO", "%d DAYS AGO", (unsigned long)(seconds / 86400)), int(seconds / 86400));
  return b;
}

void build_devices(MenuBuilder& m, Ctx&) {
  const std::vector<webpair::Device> list = webpair::devices();
  const Refresh refresh = m.refresh;
  if (list.empty()) {
    m.info(tr("NO PAIRED DEVICES"));
    return;
  }
  const long long now = static_cast<long long>(::time(nullptr));
  for (const webpair::Device& d : list) {
    const std::string id = d.id;
    m.action(d.name, [id, refresh] {
       webpair::forget(id);
       refresh();
     })
        .value(ago(now - d.last_seen))
        .confirm(tr("FORGET THIS DEVICE?"));
  }
  m.separator();
  m.action(tr("FORGET ALL DEVICES"), [refresh] {
     webpair::forget_all();
     refresh();
   })
      .danger()
      .confirm(tr("FORGET ALL DEVICES?"));
}

std::unique_ptr<Screen> make_devices() {
  MenuSpec s;
  s.title = tr("PAIRED DEVICES");
  s.title_key = "PAIRED DEVICES";
  s.build = build_devices;
  return netmenu::make_live_menu(std::move(s));  // a phone that pairs while the list is open shows up
}

void network_rows(MenuBuilder& m, Ctx&) {
  m.submenu(tr("SHOW QR CODE"), "menu.webqr").hint(tr("Scan it with the phone camera: the page opens and knows the PIN."));
  m.submenu(tr("PAIRED DEVICES"), "menu.webdevices").value(std::to_string(webpair::devices().size())).hint(tr("Phones that need no PIN."));
}

}  // namespace

ZF_REGISTER_SCREEN("menu.webqr", [](Ctx& cx, const std::string&) -> std::unique_ptr<Screen> { return std::make_unique<QrScreen>(cx); });
ZF_REGISTER_SCREEN("menu.webdevices", [](Ctx&, const std::string&) { return make_devices(); });

ZF_REGISTER_WEB_MODULE([] {
  webtransfer::WebModule m;
  m.name = "pair-console";
  m.order = 6;
  m.network_rows = network_rows;
  return m;
});

}  // namespace zf
