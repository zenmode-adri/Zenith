// RESUME LAST GAME AT STARTUP (resume_last.hpp): off by default; on + a recent game = the countdown screen, then the launch of the
// "Continue playing" row (same command); the boot game, the once-per-boot marker, a missing ROM and a very short last session each
// stop it; A starts at once, B / Start cancel. Temp dirs only; nothing is launched (dry run).
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "app.hpp"
#include "bootargs.hpp"
#include "check.hpp"
#include "i18n.hpp"
#include "menu.hpp"
#include "registry.hpp"
#include "resume_last.hpp"
#include "savestates.hpp"
#include "screens.hpp"
#include "util.hpp"

using namespace zf;
namespace ss = zf::savestates;

namespace {

std::string g_tmp, g_png;
int g_case = 0;

void put(const std::string& path, time_t mtime) {
  std::ofstream(path, std::ios::trunc | std::ios::binary) << (path.size() > 4 && path.substr(path.size() - 4) == ".png" ? g_png : std::string("x"));
  struct utimbuf u;
  u.actime = u.modtime = mtime;
  ::utime(path.c_str(), &u);
}

System make_gb() {
  System s;
  s.name = "gb";
  s.fullname = "Game Boy";
  s.theme = "gb";
  s.command = "runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR%";
  s.emulator = "retroarch";
  s.core = "gambatte";
  s.emus = {{"retroarch", {"gambatte"}}};
  s.rom_dir = g_tmp + "/roms/gb";
  s.exts = {".gb"};
  s.gamelist = s.rom_dir + "/gamelist.xml";
  for (const char* n : {"Alpha", "Bravo"}) {
    Game g;
    g.name = n;
    g.path = s.rom_dir + "/" + n + ".gb";
    s.games.push_back(std::move(g));
  }
  s.count = s.games.size();
  s.loaded = true;
  return s;
}

/// A fresh context: Alpha (autosave, newest) and Bravo (slot 1) with their ROMs on disk; `extra` is added to system.cfg.
void fresh(Ctx& cx, const std::string& extra) {
  cx.sv->cmd.dry_run = true;
  cx.w = cx.h = 720;
  cx.conf = SystemConf::from_text(extra, g_tmp + "/system.cfg");
  cx.systems = {make_gb()};
  cx.items = {SystemItem{"gb", "Game Boy", 2}};
  for (const Game& g : cx.systems[0].games) put(g.path, time(nullptr) - 99999);
  ss::invalidate();
}

resume::Paths fresh_paths() {
  ++g_case;
  const std::string d = g_tmp + "/case" + std::to_string(g_case);
  return resume::Paths{d + "/run/startup-game", d + "/last-session"};
}

bool has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

/// Counts `secs` seconds of a screen that is already armed.
void count(Screen& s, Ctx& cx, float secs, float step = 1.0f) {
  for (float t = 0; t < secs - 0.001f; t += step) s.tick(step, cx);
}

std::unique_ptr<resume::ResumeScreen> armed(std::unique_ptr<Screen> s) {
  if (!s) return nullptr;
  auto* r = static_cast<resume::ResumeScreen*>(s.release());  // built with -fno-rtti: this is the type `begin` makes
  r->arm();
  return std::unique_ptr<resume::ResumeScreen>(r);
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/zf-resume-XXXXXX";
  g_tmp = mkdtemp(tmpl);
  CHECK(read_file(std::string(ZF_TEST_DATA) + "/cover-box.png", g_png));
  const std::string states = g_tmp + "/roms/savestates/gb";
  ::mkdir((g_tmp + "/roms").c_str(), 0755);
  ::mkdir((g_tmp + "/roms/gb").c_str(), 0755);
  ::mkdir((g_tmp + "/roms/savestates").c_str(), 0755);
  ::mkdir(states.c_str(), 0755);
  put(states + "/Alpha.state.auto", time(nullptr) - 60);
  put(states + "/Alpha.state.auto.png", time(nullptr) - 60);
  put(states + "/Bravo.state1", time(nullptr) - 600);
  put(states + "/Bravo.state1.png", time(nullptr) - 600);
  const time_t now = time(nullptr);
  const std::string on = std::string(resume::kKey) + "=1\n";
  const std::string alpha = g_tmp + "/roms/gb/Alpha.gb";

  {  // off by default: no screen and not even the marker
    Ctx cx;
    fresh(cx, "");
    CHECK(!resume::enabled(cx));
    const resume::Paths p = fresh_paths();
    std::string why;
    CHECK(resume::begin(cx, p, now, why) == nullptr);
    CHECK(!file_exists(p.marker));
    CHECK(std::string(resume::kKey) == "zfront.resume_last");
  }

  {  // on + a recent game: the countdown, then the same launch as the "Continue playing" row
    Ctx cx;
    fresh(cx, on);
    CHECK(resume::enabled(cx));
    const resume::Paths p = fresh_paths();
    std::string why;
    auto s = armed(resume::begin(cx, p, now, why));
    CHECK(s != nullptr);
    if (!s) return 1;
    CHECK(file_exists(p.marker));  // once per boot: claimed
    CHECK_EQ(s->target().name, std::string("Alpha"));
    CHECK_EQ(s->target().slot, -1);
    CHECK_EQ(s->seconds_left(), 3);
    CHECK(s->closes_on_launch());
    CHECK(s->take_transition(cx).kind == Transition::None);  // nothing at once
    count(*s, cx, 2.0f);
    CHECK_EQ(s->seconds_left(), 1);
    CHECK(s->take_redraw());   // the number changed
    CHECK(!s->take_redraw());  // ...and says so once
    CHECK(s->take_transition(cx).kind == Transition::None);
    CHECK(s->wake_in().has_value() && *s->wake_in() > 0.0f && *s->wake_in() <= 1.0f);
    count(*s, cx, 1.0f);
    Transition t = s->take_transition(cx);
    CHECK(t.kind == Transition::Launch);
    CHECK(has(t.cmd, "Alpha.gb"));
    CHECK(has(t.cmd, "-autosave 1"));  // the autosave is loaded
    CHECK_EQ(t.sys, 0);
    CHECK_EQ(t.game, 0);
    ss::after_game();
    ss::invalidate();
    // the very same transition the savestate manager / the row make for that state
    Transition direct = ss::launch_state(cx, 0, 0, -1);
    ss::after_game();
    CHECK(direct.kind == Transition::Launch);
    CHECK_EQ(t.cmd, direct.cmd);
    CHECK(s->take_transition(cx).kind == Transition::Pop);  // (the launch removes the screen; if only a question had come back, this does)

    // a second start of the frontend in the same boot (systemd restart, or back from the game): the marker is there
    std::string why2;
    CHECK(resume::begin(cx, p, now, why2) == nullptr);
  }

  {  // the screen is the one the Continue-playing row would launch: same command from the carousel when a theme is at hand
    Ctx cx;
    fresh(cx, on);
    const char* e = std::getenv("ZF_THEME_DIR");
    std::string err;
    Theme th;
    if (Theme::load(e ? e : "/usr/share/zfront/theme-dark", ratio_name(720, 720), th, err) && cx.assets.load_font(th.path(th.str("font")), err)) {
      cx.th = th;
      CHECK(load_common(cx.th, cx.assets, cx.common, err));
      CarouselScreen c(0);
      for (int i = 0; i < 3; ++i) c.tick(1.0f / 60.0f, cx);
      c.handle(Action::Down, cx);
      Transition row = c.handle(Action::A, cx);  // Alpha: the autosave
      ss::after_game();
      ss::invalidate();
      CHECK(row.kind == Transition::Launch);
      std::string why;
      auto s = armed(resume::begin(cx, fresh_paths(), now, why));
      CHECK(s != nullptr);
      if (s) {
        count(*s, cx, 3.0f);
        Transition t = s->take_transition(cx);
        ss::after_game();
        CHECK(t.kind == Transition::Launch);
        CHECK_EQ(t.cmd, row.cmd);
        CHECK_EQ(t.sys, row.sys);
        CHECK_EQ(t.game, row.game);
      }
      // drawn: a title, the number, the picture and the help, and the header box is published for the layout test
      ss::invalidate();
      auto d = armed(resume::begin(cx, fresh_paths(), now, why));
      CHECK(d != nullptr);
      if (d) {
        std::vector<Op> ops;
        d->draw(cx, ops);
        CHECK(ops.size() > 6);
        bool image = false;
        for (const Op& o : ops) image = image || o.kind == Op::Image;
        CHECK(image);
      }

      // in the App: the countdown ends without a key and the launch reaches the loop through take_pending_launch
      ss::invalidate();
      Ctx cx2;
      fresh(cx2, on);
      cx2.th = th;
      CHECK(cx2.assets.load_font(th.path(th.str("font")), err));
      CHECK(load_common(cx2.th, cx2.assets, cx2.common, err));
      std::string why3;
      std::unique_ptr<Screen> rs = resume::begin(cx2, fresh_paths(), now, why3);
      CHECK(rs != nullptr);
      App app(std::move(cx2), std::make_unique<CarouselScreen>(0), make_status_source("/nonexistent"));
      app.push_screen(std::move(rs));
      app.render();  // the first frame arms the countdown
      app.tick(1.0f);
      app.tick(1.0f);
      CHECK(!app.take_pending_launch().has_value());
      app.tick(1.0f);
      std::optional<Outcome> o = app.take_pending_launch();
      CHECK(o.has_value());
      if (o) {
        CHECK(o->kind == Outcome::Launch);
        CHECK(has(o->cmd, "Alpha.gb"));
        CHECK(has(o->cmd, "-autosave 1"));
      }
      CHECK(!app.take_pending_launch().has_value());
      ss::after_game();
    } else {
      std::fprintf(stderr, "test_resume_last: theme not found (%s): carousel / draw / App part skipped\n", err.c_str());
    }
  }

  {  // the game set to start with the console wins: nothing resumes, and the marker is left to the boot game
    Ctx cx;
    fresh(cx, on + "global.bootgame.path=" + g_tmp + "/roms/gb/Bravo.gb\n");
    const resume::Paths p = fresh_paths();
    std::string why;
    CHECK(resume::begin(cx, p, now, why) == nullptr);
    CHECK(!file_exists(p.marker));
  }

  {  // this boot already started something (marker present): no resume
    Ctx cx;
    fresh(cx, on);
    const resume::Paths p = fresh_paths();
    std::string why;
    CHECK(boot::startup_game("x", p.marker, [](const std::string&) { return true; }, why) == "x");  // what the boot game does
    CHECK(file_exists(p.marker));
    CHECK(resume::begin(cx, p, now, why) == nullptr);
  }

  {  // the ROM of the newest game is gone: no resume (an older game is not put in its place), and the marker is still claimed
    Ctx cx;
    fresh(cx, on);
    ::unlink(cx.systems[0].games[0].path.c_str());
    const resume::Paths p = fresh_paths();
    std::string why;
    CHECK(resume::begin(cx, p, now, why) == nullptr);
    CHECK(file_exists(p.marker));
    CHECK(!resume::newest(cx, now).has_value());
    ::unlink((g_tmp + "/roms/gb/Alpha.gb").c_str());
  }

  {  // the game list does not hold the newest game (Kid mode loads only the kid games, a hidden game is out): nothing resumes
    Ctx cx;
    fresh(cx, on);
    cx.systems[0].games.erase(cx.systems[0].games.begin());  // Alpha: still on disk, but not in the list
    CHECK(file_exists(g_tmp + "/roms/gb/Alpha.gb"));
    std::string why;
    CHECK(resume::begin(cx, fresh_paths(), now, why) == nullptr);
  }

  {  // no recent game at all
    Ctx cx;
    fresh(cx, on);
    cx.systems[0].rom_dir = g_tmp + "/roms/empty/gb";  // its savestate folder holds nothing
    std::string why;
    CHECK(resume::begin(cx, fresh_paths(), now, why) == nullptr);
    cx.systems[0].rom_dir = g_tmp + "/roms/gb";
  }

  {  // B and Start cancel (the frontend stays), A starts at once
    for (Action a : {Action::B, Action::Start}) {
      Ctx cx;
      fresh(cx, on);
      std::string why;
      auto s = armed(resume::begin(cx, fresh_paths(), now, why));
      CHECK(s != nullptr);
      if (!s) continue;
      count(*s, cx, 1.0f);
      CHECK(s->handle(Action::Down, cx).kind == Transition::None);  // other keys do nothing
      CHECK(s->handle(a, cx).kind == Transition::Pop);
      count(*s, cx, 5.0f);
      CHECK(s->take_transition(cx).kind != Transition::Launch);  // cancelled: the time running out launches nothing
      CHECK(s->handle(Action::A, cx).kind == Transition::None);
    }
    Ctx cx;
    fresh(cx, on);
    std::string why;
    auto s = armed(resume::begin(cx, fresh_paths(), now, why));
    CHECK(s != nullptr);
    if (s) {
      Transition t = s->handle(Action::A, cx);
      CHECK(t.kind == Transition::Launch);
      CHECK(has(t.cmd, "Alpha.gb"));
      ss::after_game();
      CHECK(s->take_transition(cx).kind != Transition::Launch);  // once
    }
  }

  {  // A asked for the launch but only nothing (or, with the missing-BIOS check, a question that NO closed) came back: the screen leaves by itself
    Ctx cx;
    fresh(cx, on);
    std::string why;
    auto s = armed(resume::begin(cx, fresh_paths(), now, why));
    CHECK(s != nullptr);
    if (s) {
      cx.systems[0].command.clear();  // no command: launch_state gives nothing
      CHECK(s->handle(Action::A, cx).kind == Transition::Pop);
      CHECK(s->take_transition(cx).kind == Transition::Pop);  // asked again (as when the BIOS question is answered NO): never none and frozen
      CHECK(s->wake_in().has_value());
    }
  }

  {  // before the first frame the clock does not run, and a stalled loop counts one second at most
    Ctx cx;
    fresh(cx, on);
    std::string why;
    std::unique_ptr<Screen> raw = resume::begin(cx, fresh_paths(), now, why);
    CHECK(raw != nullptr);
    if (raw) {
      raw->tick(10.0f, cx);
      CHECK(raw->take_transition(cx).kind == Transition::None);
      auto* r = static_cast<resume::ResumeScreen*>(raw.get());
      CHECK_EQ(r->seconds_left(), 3);
      r->arm();
      raw->tick(10.0f, cx);  // a long stall
      CHECK_EQ(r->seconds_left(), 2);
      CHECK(raw->take_transition(cx).kind == Transition::None);
    }
  }

  {  // a last session under 10 s (probably a crash on start) blocks it until a longer one replaces it
    Ctx cx;
    fresh(cx, on);
    {
      const resume::Paths p = fresh_paths();
      resume::note_session(p.session, 4, alpha);
      unsigned long secs = 99;
      CHECK(!resume::session_allows(p.session, alpha, secs));
      CHECK_EQ(secs, 4ul);
      std::string why;
      CHECK(resume::begin(cx, p, now, why) == nullptr);
      CHECK(!why.empty());
      CHECK(file_exists(p.marker));  // decided for this boot
    }
    {
      const resume::Paths p = fresh_paths();
      resume::note_session(p.session, 9, alpha);
      std::string why;
      CHECK(resume::begin(cx, p, now, why) == nullptr);
    }
    {  // a short run of ANOTHER thing (a tool, another game) after a long play does not block the resume of the game played before
      const resume::Paths p = fresh_paths();
      resume::note_session(p.session, 2, g_tmp + "/roms/gb/Bravo.gb");
      unsigned long secs = 0;
      CHECK(resume::session_allows(p.session, alpha, secs));
      std::string why;
      CHECK(resume::begin(cx, p, now, why) != nullptr);
    }
    {  // a line without a game (written by an unknown launch) blocks like the game's own
      const resume::Paths p = fresh_paths();
      resume::note_session(p.session, 3, "");
      std::string why;
      CHECK(resume::begin(cx, p, now, why) == nullptr);
    }
    {
      const resume::Paths p = fresh_paths();
      resume::note_session(p.session, 10, alpha);  // exactly the limit is fine
      unsigned long secs = 0;
      CHECK(resume::session_allows(p.session, alpha, secs));
      std::string why;
      CHECK(resume::begin(cx, p, now, why) != nullptr);
    }
    {
      const resume::Paths p = fresh_paths();
      resume::note_session(p.session, 4, alpha);
      resume::note_session(p.session, 3600, alpha);  // a long session replaced it
      std::string why;
      CHECK(resume::begin(cx, p, now, why) != nullptr);
    }
    {  // no file, or garbage in it: allowed (a console that never recorded one must resume)
      const resume::Paths p = fresh_paths();
      unsigned long secs = 7;
      CHECK(resume::session_allows(p.session, alpha, secs));
      { std::ofstream(g_tmp + "/junk") << "abc\n"; }
      CHECK(resume::session_allows(g_tmp + "/junk", alpha, secs));
    }
  }

  {  // the GAMES menu offers the switch, off by default, and it writes the key
    Ctx cx;
    fresh(cx, "");
    std::string err;
    cx.es = EsSettings::from_text("<config/>", g_tmp + "/es_settings.cfg");
    std::unique_ptr<Screen> s = Registry::instance().make("menu.game", cx, "");
    CHECK(s != nullptr);
    if (s) {
      Menu& m = *static_cast<Menu*>(s.get());
      m.rebuild(cx);
      int row = -1;
      for (size_t i = 0; i < m.size(); ++i)
        if (m.entry(i).label == std::string(tr("RESUME LAST GAME AT STARTUP"))) row = int(i);
      CHECK(row >= 0);
      if (row >= 0) {
        for (size_t i = 0; i < m.size() + 1 && m.cursor() != size_t(row); ++i) m.handle(Action::Down, cx);
        m.handle(Action::A, cx);
        CHECK(resume::enabled(cx));
        std::string text;
        read_file(g_tmp + "/system.cfg", text);
        CHECK(has(text, "zfront.resume_last=1"));
        m.handle(Action::A, cx);
        CHECK(!resume::enabled(cx));
        read_file(g_tmp + "/system.cfg", text);
        CHECK(!has(text, "zfront.resume_last"));
      }
    }
  }

  return zt::done();
}
