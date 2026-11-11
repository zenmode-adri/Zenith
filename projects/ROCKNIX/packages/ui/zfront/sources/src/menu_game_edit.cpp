#include "menu_game_edit.hpp"

#include <cstdio>
#include <memory>

#include "gamemeta.hpp"
#include "i18n.hpp"
#include "menu_game.hpp"
#include "menu_startup.hpp"
#include "registry.hpp"
#include "scraper_run.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

namespace zf::mg {
namespace {

long system_index(const Ctx& cx, const std::string& name) {
  for (size_t i = 0; i < cx.systems.size(); ++i)
    if (cx.systems[i].name == name) return long(i);
  return -1;
}

// What the editor holds between its rows: ES edits a copy of the metadata and writes it on SAVE.
struct Draft {
  std::string system, path;
  long sys = -1;
  GameForm base, cur;  // what the gamelist has / what the rows show
  unsigned gen = 0;    // `games_generation` when it was read
  bool loaded = false;
  bool leave = false;  // set by the "save changes?" answer: the menu closes on its next tick
  bool leave_all = false;  // the answer was to Start: every menu closes, not only this one

  bool dirty() const { return base != cur; }
  void reload(const Ctx& cx) {
    GameForm f;
    if (sys >= 0 && game_form_load(cx, size_t(sys), path, f)) {
      base = cur = f;
      loaded = true;
    }
    gen = games_generation();
  }
};

void save_draft(Ctx& cx, Draft& d) {
  if (d.sys < 0 || !d.dirty()) return;
  game_edit(cx, size_t(d.sys), d.path, d.base, d.cur);
  d.base = d.cur;
  d.gen = games_generation();
}

std::string one_line(std::string s, size_t max = 48) {
  for (char& c : s)
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  if (s.size() > max) {
    size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;  // not inside a UTF-8 sequence
    s = s.substr(0, cut) + "...";
  }
  return s;
}

class EditMenu : public Menu {
 public:
  EditMenu(MenuSpec spec, std::shared_ptr<Draft> d) : Menu(std::move(spec)), d_(std::move(d)) {}

  Transition handle(Action a, Ctx& cx) override {
    if ((a == Action::B || a == Action::Start) && d_->dirty()) {  // ES: "SAVE CHANGES?" YES saves and closes, NO closes, B stays
      std::shared_ptr<Draft> d = d_;
      const bool all = a == Action::Start;
      return ui::choose(tr("SAVE CHANGES?"), {{"yes", tr("YES")}, {"no", tr("NO")}}, "yes", [d, all](Ctx& c, const std::string& v) {
        if (v == "yes") save_draft(c, *d);
        d->leave = true;
        d->leave_all = all;
      });
    }
    return Menu::handle(a, cx);
  }

  bool tick(float dt, Ctx& cx) override {
    bool r = Menu::tick(dt, cx);
    if (!d_->dirty() && d_->gen != games_generation()) {  // a scrape or an edit changed the game under the rows
      d_->reload(cx);
      rebuild(cx);
      r = true;
    }
    return r;
  }

  Transition take_transition(Ctx& cx) override {
    (void)cx;
    if (d_->leave) {
      d_->leave = false;
      const bool all = d_->leave_all;
      d_->leave_all = false;
      return all ? Transition::close_menus() : Transition::pop();
    }
    return Transition::none();
  }

 private:
  std::shared_ptr<Draft> d_;
};

void build_edit(MenuBuilder& m, Ctx& cx, const std::shared_ptr<Draft>& d) {
  if (!d->loaded) {
    m.info(tr("UNKNOWN GAME"));
    return;
  }
  // The labels and prompts are ES's (its catalog translates them), as written there ("Name", "Nombre"): upper case they stood out among the
  // sentence-case rows of the other menus (seen on the device, 9-nov)
  auto text_row = [&](const std::string& label, std::string GameForm::*field, const std::string& prompt, size_t max_len) {
    Refresh rf = m.refresh;
    m.action(label, [d, field, prompt, max_len, rf] {
       ui::KeyboardOpts o;
       o.title = prompt;
       o.max_len = max_len;
       return ui::keyboard(o, d->cur.*field, [d, field, rf](Ctx&, const std::string& v) {
         d->cur.*field = v;
         if (rf) rf();
       });
     }).value(one_line(d->cur.*field));
  };
  text_row(std::string(tr("Name")), &GameForm::name, tr("this game's name"), 128);
  text_row(std::string(tr("Description")), &GameForm::desc, tr("this game's description"), 2000);
  text_row(std::string(tr("Genre")), &GameForm::genre, tr("enter game genre"), 128);
  text_row(std::string(tr("Sort name")), &GameForm::sortname, tr("enter game sort name"), 128);  // ES MetaData.cpp:39
  text_row(std::string(tr("Developer")), &GameForm::developer, tr("this game's developer"), 128);
  text_row(std::string(tr("Publisher")), &GameForm::publisher, tr("this game's publisher"), 128);
  text_row(std::string(tr("Players")), &GameForm::players, tr("this game's number of players"), 32);
  {
    Refresh rf = m.refresh;
    m.action(std::string(tr("Release date")), [d, rf] {
       ui::KeyboardOpts o;
       o.title = tr("enter release date");
       o.max_len = 10;
       return ui::keyboard(o, d->cur.releasedate, [d, rf](Ctx& c, const std::string& v) {
         if (!valid_release_date(v)) {  // YYYY-MM-DD
           c.sv->toasts.push(tr("INVALID DATE"), 3000);
           return;
         }
         d->cur.releasedate = v;
         if (rf) rf();
       });
     }).value(d->cur.releasedate.empty() ? std::string("--") : d->cur.releasedate);
  }
  m.slider(std::string(tr("Rating")), 0, 10, 1, [d] { return d->cur.rating; }, [d](int v) { d->cur.rating = v; }).format([](int v) {
    char b[24];
    std::snprintf(b, sizeof b, "%d.%d / 5", v / 2, (v % 2) * 5);  // five stars in half steps; ES keeps it as 0..1
    return std::string(b);
  });
  m.toggle(std::string(tr("Favorite")), [d] { return d->cur.favorite; }, [d](bool v) { d->cur.favorite = v; });
  m.toggle(std::string(tr("Hidden")), [d] { return d->cur.hidden; }, [d](bool v) { d->cur.hidden = v; });
  m.toggle(std::string(tr("Kidgame")), [d] { return d->cur.kidgame; }, [d](bool v) { d->cur.kidgame = v; });  // ES MetaData.cpp:83: what Kid mode shows

  m.action(tr("SAVE"), [d](Ctx& c) {
    save_draft(c, *d);
    return Transition::pop();
  });
  if (d->sys >= 0 && scraper::system_supported(cx, size_t(d->sys))) {
    m.action(tr("SCRAPE"), [d](Ctx& c) {
      save_draft(c, *d);  // the scrape writes to the gamelist itself: what is pending goes first, the rows read it again afterwards
      return Transition::open("menu.scraper.game", game_arg(d->system, d->path));
    });
  }
  std::string system = d->system, path = d->path;
  m.action(tr("DELETE"), [system, path](Ctx& c) { return delete_game_now(c, system, path); })
      .danger()
      .confirm(tr("THIS WILL DELETE THE ACTUAL GAME FILE(S)!\nARE YOU SURE?"));
}

}  // namespace

Transition delete_game_now(Ctx& cx, const std::string& system, const std::string& rom_path) {
  long si = system_index(cx, system);
  if (si < 0) return Transition::none();
  if (is_startup_game(cx, rom_path)) clear_startup_game(cx);
  std::string why;
  if (game_delete(cx, size_t(si), rom_path, why) != DeleteResult::Ok) {
    cx.sv->toasts.push(tr("THE GAME COULD NOT BE DELETED"), 4000);
    return Transition::none();
  }
  return Transition::close_menus();  // the game is gone: the menus about it and the list row go with it
}

void edit_game_entries(MenuBuilder& m, Ctx& cx, const std::string& system, const std::string& rom_path) {
  (void)cx;
  m.submenu(tr("EDIT THIS GAME'S METADATA"), "menu.game.edit", game_arg(system, rom_path));
  m.action(tr("DELETE GAME"), [system, rom_path](Ctx& c) { return delete_game_now(c, system, rom_path); })
      .danger()
      .confirm(tr("THIS WILL DELETE THE ACTUAL GAME FILE(S)!\nARE YOU SURE?"));
}

ZF_REGISTER_SCREEN("menu.game.edit", [](Ctx& cx, const std::string& arg) -> std::unique_ptr<Screen> {
  auto d = std::make_shared<Draft>();
  split_game_arg(arg, d->system, d->path);
  d->sys = system_index(cx, d->system);
  d->reload(cx);
  MenuSpec spec;
  spec.title = tr("EDIT METADATA");
  spec.title_key = N_("EDIT METADATA");
  spec.build = [d](MenuBuilder& m, Ctx& c) { build_edit(m, c, d); };
  return std::make_unique<EditMenu>(std::move(spec), d);
});

}  // namespace zf::mg
