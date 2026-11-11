#include <algorithm>
// ES's USER INTERFACE MODE (uimode.hpp): which mode the settings mean, the unlock code, which screens a locked console opens, the main
// menu of Kiosk and Kid, the unlock screen and the mode picker, and the Kid filter of the game lists (`<kidgame>`).
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "app.hpp"
#include "check.hpp"
#include "menu.hpp"
#include "registry.hpp"
#include "settings.hpp"
#include "systems.hpp"
#include "uimode.hpp"
#include "util.hpp"

using namespace zf;
using uimode::Mode;

namespace {

const char* kVars =
    "<theme><variables><mx>0.02813</mx><avY>0.06667</avY><fMenuTitle>0.03834</fMenuTitle><fMenu>0.0375</fMenu><fMenuSmall>0.03021</fMenuSmall>"
    "<fMenuFoot>0.02708</fMenuFoot><fHelp>0.02542</fHelp><helpY>0.94792</helpY><listLines>9</listLines><listPosY>0.1375</listPosY><listH>0.7125</listH>"
    "<selH>0.0625</selH><volW>0.34687</volW><volH>0.07917</volH><volY>0.06667</volY><fVol>0.02917</fVol><bg>26262A</bg><txt>F0F0F2</txt><dim>BEBEC6</dim>"
    "<dimDark>B4B4BC</dimDark><cyan>00C3E3</cyan><cyanDark>4FD8F0</cyanDark><cyanSoft>22525F</cyanSoft></variables></theme>";

std::string g_tmp;

std::string es_xml(const std::string& body) { return "<?xml version=\"1.0\"?>\n<config>\n" + body + "</config>\n"; }

void make_ctx(Ctx& cx, const std::string& es_body) {
  std::string err;
  CHECK(Theme::from_xml(kVars, "<theme/>", "4/3", cx.th, err));
  cx.w = 640, cx.h = 480;
  cx.sv->cmd.dry_run = true;
  cx.es = EsSettings::from_text(es_xml(es_body), g_tmp + "/es_settings.cfg");
  uimode::apply(cx.es);
}

std::string str(const char* name, const char* v) { return std::string("<string name=\"") + name + "\" value=\"" + v + "\" />\n"; }

Menu* open_menu(std::unique_ptr<Screen>& hold, Ctx& cx, const char* name) {
  hold = Registry::instance().make(name, cx, "");
  if (!hold) return nullptr;
  Menu* m = static_cast<Menu*>(hold.get());
  m->tick(0.0f, cx);
  return m;
}

std::vector<std::string> labels(const Menu& m) {
  std::vector<std::string> v;
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label != "WIDGET DEMO") v.push_back(m.entry(i).label);  // the dry-run menu adds it
  return v;
}

bool has(const Menu& m, const std::string& l) {
  for (size_t i = 0; i < m.size(); ++i)
    if (m.entry(i).label == l) return true;
  return false;
}

void put(const std::string& p, const std::string& t) {
  std::ofstream f(p, std::ios::binary);
  f << t;
}

bool feed_all(uimode::Unlock& u, const std::vector<Action>& keys, bool invert) {
  bool r = false;
  for (Action a : keys) r = u.feed(a, invert);
  return r;
}

}  // namespace

int main() {
  g_tmp = "/tmp/zf_test_uimode_" + std::to_string(getpid());
  ::mkdir(g_tmp.c_str(), 0755);

  // ---- which mode the settings mean (ES UIModeController)
  {
    CHECK(uimode::of(EsSettings::from_text(es_xml(""))) == Mode::Full);
    CHECK(uimode::of(EsSettings::from_text(es_xml(str("UIMode", "Full")))) == Mode::Full);
    CHECK(uimode::of(EsSettings::from_text(es_xml(str("UIMode", "Kiosk")))) == Mode::Kiosk);
    CHECK(uimode::of(EsSettings::from_text(es_xml(str("UIMode", "Kid")))) == Mode::Kid);
    CHECK(uimode::of(EsSettings::from_text(es_xml(str("UIMode", "Basic")))) == Mode::Kiosk);  // never open
    CHECK(uimode::of(EsSettings::from_text(es_xml(str("UIMode", "whatever")))) == Mode::Kiosk);
    const std::string kid = "<bool name=\"ForceKid\" value=\"true\" />\n", kiosk = "<bool name=\"ForceKiosk\" value=\"true\" />\n";
    CHECK(uimode::of(EsSettings::from_text(es_xml(kid))) == Mode::Kid);
    CHECK(uimode::of(EsSettings::from_text(es_xml(kiosk))) == Mode::Kiosk);
    CHECK(uimode::of(EsSettings::from_text(es_xml(kid + str("UIMode", "Kiosk")))) == Mode::Kid);   // ES: ForceKid wins over Kiosk
    CHECK(uimode::of(EsSettings::from_text(es_xml(kiosk + str("UIMode", "Kid")))) == Mode::Kiosk);  // ... and ForceKiosk over Kid
    CHECK(uimode::of(EsSettings::from_text(es_xml(kiosk + kid))) == Mode::Kid);                      // both: the stricter
  }

  // ---- the code
  {
    CHECK_EQ(uimode::passkey(EsSettings::from_text(es_xml(""))), std::string("aaaba"));
    CHECK_EQ(uimode::passkey(EsSettings::from_text(es_xml(str("UIMode_passkey", "uuddlrlrba")))), std::string("uuddlrlrba"));
    CHECK_EQ(uimode::passkey_text("aaaba"), std::string("A, A, A, B, A"));
    CHECK_EQ(uimode::passkey_text("udlrxy"), std::string("\xE2\x86\x91, \xE2\x86\x93, \xE2\x86\x90, \xE2\x86\x92, X, Y"));
    const std::vector<Action> code = {Action::A, Action::A, Action::A, Action::B, Action::A};
    {
      uimode::Unlock u("aaaba");
      CHECK(!u.feed(Action::A, true));
      CHECK_EQ(u.progress(), size_t(1));
      CHECK(feed_all(u, {Action::A, Action::A, Action::B, Action::A}, true));  // the last key completes it
      CHECK_EQ(u.progress(), size_t(0));
    }
    {
      uimode::Unlock u("aaaba");
      CHECK(!feed_all(u, {Action::A, Action::A, Action::Start, Action::A, Action::B, Action::A}, true));  // Start is not part of it: starts over
      CHECK_EQ(u.progress(), size_t(1));  // the trailing "A": one key into the code, as ES (a failed key is not retried)
      uimode::Unlock w("aaaba");
      // a, a, a, a: the 4th is not 'b', so ES resets to 0 without counting that 'a' again
      CHECK(!feed_all(w, {Action::A, Action::A, Action::A, Action::A, Action::B, Action::A}, true));
    }
    {
      uimode::Unlock u("aaaba");  // InvertButtons off: the pad's "a" is the logical B
      CHECK(feed_all(u, {Action::B, Action::B, Action::B, Action::A, Action::B}, false));
      uimode::Unlock w("aaaba");
      CHECK(!feed_all(w, code, false));
    }
    {
      uimode::Unlock u("uuddlrlrba");
      CHECK(feed_all(u, {Action::Up, Action::Up, Action::Down, Action::Down, Action::Left, Action::Right, Action::Left, Action::Right, Action::B, Action::A}, true));
    }
  }

  // ---- screens a locked console may open
  {
    for (const char* ok : {"menu.main", "menu.system.info", "menu.quit", "menu.uimode.unlock", "menu.savestates", "menu.controllers.detect"}) {
      CHECK(uimode::screen_allowed(Mode::Kid, ok));
      CHECK(uimode::screen_allowed(Mode::Kiosk, ok));
    }
    for (const char* no : {"menu.game", "menu.game_options", "menu.system", "menu.g.system", "menu.controllers", "menu.network", "menu.scraper", "menu.uimode",
                           "menu.retroachievements", "menu.collections.game", "menu.system.reset", "menu.battery"}) {
      CHECK(!uimode::screen_allowed(Mode::Kid, no));
      CHECK(!uimode::screen_allowed(Mode::Kiosk, no));
      CHECK(uimode::screen_allowed(Mode::Full, no));
    }
  }

  // ---- the registry is the door: nothing but those opens in a restricted mode
  {
    Ctx cx;
    make_ctx(cx, str("UIMode", "Kid"));
    CHECK(Registry::instance().has("menu.game_options"));  // registered...
    CHECK(Registry::instance().make("menu.game_options", cx, "x\ty") == nullptr);  // ...but not opened
    CHECK(Registry::instance().make("menu.system", cx, "") == nullptr);
    CHECK(Registry::instance().make("menu.uimode", cx, "") == nullptr);
    CHECK(Registry::instance().make("menu.main", cx, "") != nullptr);
    CHECK(Registry::instance().make("menu.quit", cx, "") != nullptr);
    // every screen, also those other modules add later (metadata editor, delete, scrape one game, list options, collections...):
    // only the six of `screen_allowed` open, in Kid and in Kiosk
    const std::vector<std::string> ok = {"menu.main", "menu.system.info", "menu.quit", "menu.uimode.unlock", "menu.savestates",
                                         "menu.controllers.detect"};
    const std::vector<std::string> all = Registry::instance().names();
    CHECK(all.size() > 20);
    for (const char* must : {"menu.game.edit", "menu.scraper.game", "menu.game_options", "menu.collections.game", "menu.ui"})
      CHECK(std::find(all.begin(), all.end(), must) != all.end());
    for (const char* mode : {"Kid", "Kiosk"}) {
      make_ctx(cx, str("UIMode", mode));
      for (const std::string& n : all) {
        const bool allowed = std::find(ok.begin(), ok.end(), n) != ok.end();
        if (uimode::screen_allowed(uimode::of(cx.es), n) != allowed) std::fprintf(stderr, "test_uimode: %s in %s\n", n.c_str(), mode);
        CHECK(uimode::screen_allowed(uimode::of(cx.es), n) == allowed);
        if (!allowed) CHECK(Registry::instance().make(n, cx, "x\ty") == nullptr);
      }
    }
    make_ctx(cx, "");
    CHECK(Registry::instance().make("menu.system", cx, "") != nullptr);
    CHECK(Registry::instance().make("menu.uimode", cx, "") != nullptr);
  }

  // ---- main menu
  {
    Ctx cx;
    std::unique_ptr<Screen> h;
    make_ctx(cx, str("UIMode", "Kiosk"));
    Menu* m = open_menu(h, cx, "menu.main");
    CHECK(m != nullptr);
    if (m) {
      CHECK_EQ(labels(*m), (std::vector<std::string>{"INFORMATION", "UNLOCK USER INTERFACE MODE", "QUIT"}));
    }
    make_ctx(cx, str("UIMode", "Kid"));
    m = open_menu(h, cx, "menu.main");
    CHECK(m != nullptr);
    if (m) CHECK_EQ(labels(*m), (std::vector<std::string>{"INFORMATION", "UNLOCK USER INTERFACE MODE", "QUIT"}));
    make_ctx(cx, "");
    m = open_menu(h, cx, "menu.main");
    CHECK(m != nullptr);
    if (m) {
      CHECK(has(*m, "GAMES"));
      CHECK(has(*m, "SYSTEM"));
      CHECK(!has(*m, "UNLOCK USER INTERFACE MODE"));
    }
  }

  // ---- the unlock screen: the code goes back to Full, saved in es_settings.cfg
  {
    Ctx cx;
    std::unique_ptr<Screen> h;
    make_ctx(cx, str("UIMode", "Kid"));
    put(g_tmp + "/es_settings.cfg", es_xml(str("UIMode", "Kid")));
    Menu* m = open_menu(h, cx, "menu.uimode.unlock");
    CHECK(m != nullptr);
    if (m) {
      CHECK_EQ(m->title(), std::string("UNLOCK USER INTERFACE MODE"));
      for (Action a : {Action::A, Action::A, Action::B, Action::A})  // wrong: B is not the 3rd key
        CHECK(m->handle(a, cx).kind == Transition::None);
      CHECK(uimode::of(cx.es) == Mode::Kid);
      CHECK(m->handle(Action::B, cx).kind == Transition::None);  // B never closes it: it is part of the code
      for (Action a : {Action::A, Action::A, Action::A, Action::B}) CHECK(m->handle(a, cx).kind == Transition::None);
      Transition t = m->handle(Action::A, cx);
      CHECK(t.kind == Transition::CloseMenus);
      CHECK(uimode::of(cx.es) == Mode::Full);
      CHECK(uimode::current() == Mode::Full);
      std::string saved;
      read_file(g_tmp + "/es_settings.cfg", saved);
      CHECK(saved.find("name=\"UIMode\" value=\"Full\"") != std::string::npos);
    }
    make_ctx(cx, str("UIMode", "Kiosk"));
    m = open_menu(h, cx, "menu.uimode.unlock");
    if (m) CHECK(m->handle(Action::Start, cx).kind == Transition::Pop);  // zfront's own way out
  }

  // ---- the picker: a restricted mode asks first, Full does not
  {
    Ctx cx;
    std::unique_ptr<Screen> h;
    make_ctx(cx, "");
    put(g_tmp + "/es_settings.cfg", es_xml(""));
    Menu* m = open_menu(h, cx, "menu.uimode");
    CHECK(m != nullptr);
    if (m) {
      CHECK_EQ(labels(*m), (std::vector<std::string>{"Full", "Kiosk", "Kid"}));
      CHECK_EQ(m->cursor(), 0);
      m->handle(Action::Down, cx);
      m->handle(Action::Down, cx);  // Kid
      Transition t = m->handle(Action::A, cx);
      CHECK(t.kind == Transition::Push && t.screen);  // the warning with the code
      CHECK(uimode::of(cx.es) == Mode::Full);          // nothing locked yet
      if (t.screen) {
        t.screen->handle(Action::Left, cx);  // the focus starts on NO
        Transition y = t.screen->handle(Action::A, cx);
        CHECK(y.kind == Transition::CloseMenus);
      }
      CHECK(uimode::of(cx.es) == Mode::Kid);
      CHECK(uimode::current() == Mode::Kid);
    }
    make_ctx(cx, str("UIMode", "Kid"));  // back to Full needs no question
    uimode::set_mode(cx.es, Mode::Full);
    CHECK(uimode::of(cx.es) == Mode::Full);
  }

  // ---- Kid: only <kidgame>true</kidgame>, and systems without such games are not loaded; Kiosk and Full unchanged
  {
    const std::string root = g_tmp + "/roms";
    ::mkdir(root.c_str(), 0755);
    for (const char* d : {"gb", "nes"}) {
      ::mkdir((root + "/" + d).c_str(), 0755);
    }
    for (const char* f : {"/gb/a.gb", "/gb/b.gb", "/gb/c.gb", "/nes/x.nes"}) put(root + f, "x");
    put(root + "/gb/gamelist.xml",
        "<gameList><game><path>./a.gb</path><name>Alpha</name><kidgame>true</kidgame></game>"
        "<game><path>./b.gb</path><name>Beta</name><kidgame>false</kidgame></game></gameList>");  // c.gb: no metadata at all
    put(root + "/nes/gamelist.xml", "<gameList><game><path>./x.nes</path><name>X</name></game></gameList>");
    const std::string xml =
        "<systemList>"
        "<system><name>gb</name><fullname>Game Boy</fullname><path>/storage/roms/gb</path><extension>.gb</extension><command>c</command></system>"
        "<system><name>nes</name><fullname>NES</fullname><path>/storage/roms/nes</path><extension>.nes</extension><command>c</command></system>"
        "</systemList>";
    std::vector<System> v;
    std::string err;

    uimode::apply(EsSettings::from_text(es_xml(str("UIMode", "Kid"))));
    CHECK(load_systems_text(xml, root, true, v, err));
    CHECK_EQ(v.size(), size_t(1));  // nes has no kid game: gone
    if (v.size() == 1) {
      CHECK_EQ(v[0].name, std::string("gb"));
      CHECK_EQ(v[0].games.size(), size_t(1));
      CHECK_EQ(v[0].count, size_t(1));
      if (!v[0].games.empty()) CHECK_EQ(v[0].games[0].name, std::string("Alpha"));
    }
    CHECK(load_systems_text(xml, root, false, v, err));  // counted: the count must be the kid games too
    CHECK_EQ(v.size(), size_t(1));
    if (v.size() == 1) CHECK_EQ(v[0].count, size_t(1));
    CHECK(load_systems_text_lazy(xml, root, v, err, true));  // LoadEmptySystems is Full only
    CHECK_EQ(v.size(), size_t(1));

    uimode::apply(EsSettings::from_text(es_xml(str("UIMode", "Kiosk"))));
    CHECK(load_systems_text(xml, root, true, v, err));
    CHECK_EQ(v.size(), size_t(2));
    if (!v.empty()) CHECK_EQ(v[0].games.size(), size_t(3));

    uimode::apply(EsSettings::from_text(es_xml("")));
    CHECK(load_systems_text(xml, root, true, v, err));
    CHECK_EQ(v.size(), size_t(2));
    if (!v.empty()) CHECK_EQ(v[0].games.size(), size_t(3));
  }

  std::string cmd = "rm -rf " + g_tmp;
  if (std::system(cmd.c_str()) != 0) {
  }
  return zt::done();
}
