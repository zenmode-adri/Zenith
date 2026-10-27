// menu-game: `menu.game` (GAME SETTINGS, the `global.*` keys of system.cfg), the list of systems with their own options
// (`menu.game.systems`), AI game translation (`menu.game.ai`) and the missing BIOS check (`menu.game.bios`).
// ES: `GuiMenu::openGamesSettings`, `GuiBios`.
#include <algorithm>
#include <memory>

#include "i18n.hpp"
#include "menu_game_int.hpp"
#include "registry.hpp"
#include "util.hpp"

namespace zf::mg {
namespace {

using namespace detail;

std::unique_ptr<Screen> make_game() {
  MenuSpec spec;
  spec.title = tr("GAMES");
  spec.title_key = N_("GAMES");
  spec.build = [](MenuBuilder& m, Ctx& cx) {
    const FeatureDb& db = features();
    Sec s(m);

    // bios
    if (bin_exists("rocknix-systems")) {
      s.header(tr("BIOS SETTINGS"));
      s.b().submenu(tr("MISSING BIOS CHECK"), "menu.game.bios");
      Ctx* c = &cx;
      s.b().toggle(tr("CHECK BIOS FILES BEFORE RUNNING A GAME"), [c] { return c->es.get_bool("CheckBiosesAtLaunch", false); },
                   [c](bool on) {
                     c->es.set_bool("CheckBiosesAtLaunch", on);
                     std::string err;
                     if (!c->es.save(&err)) c->sv->toasts.push(tr("Could not save the setting"), 3000);
                   });
    }

    // savestates
    s.header(tr("SAVESTATES"));
    conf_switch(s.b(), cx, tr("AUTO SAVE/LOAD"), "global.autosave", "1", "").hint(tr("Load latest savestate on game launch and savestate when exiting game."));
    conf_choice(s.b(), cx, tr("INCREMENTAL SAVESTATES"),
                "global.incrementalsavestates", {{"", tr("INCREMENT PER SAVE")}, {"0", tr("INCREMENT SLOT")}, {"2", tr("DO NOT INCREMENT")}}, "")
        .hint(tr("Never overwrite old savestates, always make new ones."));
    conf_choice(s.b(), cx, tr("SHOW SAVESTATE MANAGER"), "global.savestates", {{"auto", tr("NO")}, {"1", tr("ALWAYS")}, {"2", tr("IF NOT EMPTY")}}, "auto")
        .hint(tr("Display savestate manager before launching a game."));

    // defaults for every game
    s.header(tr("DEFAULT GLOBAL SETTINGS"));
    if (!db.has_global("ratio")) {
      std::vector<Option> o;
      for (const auto& r : ratios()) o.push_back({r.second, r.first[0] >= 'A' && r.first[0] <= 'Z' ? tr(r.first) : r.first});
      conf_choice(s.b(), cx, tr("GAME ASPECT RATIO"), "global.ratio", o, "auto").hint(tr("Force the game to render in this aspect ratio."));
    }
    if (!db.has_global("smooth")) conf_tri(s.b(), cx, tr("SMOOTH GAMES (BILINEAR FILTERING)"), "global.smooth");
    if (!db.has_global("rewind")) conf_tri(s.b(), cx, tr("REWIND"), "global.rewind").hint(tr("Store past states to rewind to in realtime, if the core supports it."));
    if (!db.has_global("integerscale")) conf_tri(s.b(), cx, tr("INTEGER SCALING (PIXEL PERFECT)"), "global.integerscale");
    if (!db.has_global("shaderset") && is_dir(env().rooted(env().shaders_dir))) {
      bool l;
      conf_choice(s.b(), cx, tr("SHADER SET"), "global.shaderset", shader_options(&l), "auto");
    }
    if (!db.has_global("videofilter") && is_dir(env().rooted(env().filters_dir))) {
      bool l;
      // the shell reads `videofilter` (setsettings.sh); ES's own menu writes `videofilters`, which nothing reads
      conf_choice(s.b(), cx, tr("VIDEO FILTER"), "global.videofilter", filter_options(&l), "auto");
    }
    if (!db.has_global("bezel")) {
      std::vector<Option> o = decoration_options();
      if (o.size() > 2) conf_choice(s.b(), cx, tr("DECORATION SET"), "global.bezel", o, "");
    }
    if (!db.has_global("runahead")) s.b().submenu(tr("LATENCY REDUCTION"), "menu.game.latency", "global");
    if (!db.has_global("ai_service_enabled")) s.b().submenu(tr("AI GAME TRANSLATION"), "menu.game.ai");

    add_features(s, cx, db.global(), "global", tr("DEFAULT GLOBAL SETTINGS"), true, feat_arg('g', std::string(), std::string(), std::string()));

    s.header(tr("SYSTEM SETTINGS"));
    s.b().submenu(tr("PER SYSTEM ADVANCED CONFIGURATION"), "menu.game.systems");
  };
  spec.on_open = [](Ctx& cx, const Refresh& refresh) { ensure_lists(cx, refresh, true, true); };
  return make_menu(std::move(spec));
}

std::unique_ptr<Screen> make_systems() {
  MenuSpec spec;
  spec.title = tr("PER SYSTEM ADVANCED CONFIGURATION");
  spec.title_key = N_("PER SYSTEM ADVANCED CONFIGURATION");
  spec.build = [](MenuBuilder& m, Ctx& cx) {
    const FeatureDb& db = features();
    bool any = false;
    for (const System& s : cx.systems) {
      if (!db.system_has_features(s) && !has_emulator_selection(s)) continue;
      m.submenu(s.fullname.empty() ? s.name : s.fullname, "menu.system_options", s.name);
      any = true;
    }
    if (!any) m.info(tr("NO OPTIONS"), std::string());
  };
  return make_menu(std::move(spec));
}

std::unique_ptr<Screen> make_ai() {
  MenuSpec spec;
  spec.title = tr("AI GAME TRANSLATION");
  spec.title_key = N_("AI GAME TRANSLATION");
  spec.build = [](MenuBuilder& m, Ctx& cx) {
    Ctx* c = &cx;
    m.toggle(tr("ENABLE AI TRANSLATION SERVICE"), [c] { return conf_get(*c, "global.ai_service_enabled") == "1"; },
             [c](bool on) { conf_put(*c, "global.ai_service_enabled", on ? "1" : "0"); });
    // native names, not translated; the order is the one of ES
    static const std::pair<const char*, const char*> langs[] = {
        {"En", "ENGLISH"}, {"Fr", "FRANÇAIS"}, {"Pt", "PORTUGUÊS"}, {"De", "DEUTSCH"}, {"El", "ΕΛΛΗΝΙΚΆ"}, {"Es", "ESPAÑOL"}, {"Cs", "ČEŠTINA"},
        {"Da", "DANSK"},   {"Hr", "HRVATSKI"}, {"Hu", "MAGYAR"},    {"It", "ITALIANO"}, {"Ja", "日本語"},    {"Ko", "한국어"},   {"Nl", "NEDERLANDS"},
        {"Nn", "NORSK"},   {"Pl", "POLSKI"},   {"Ro", "ROMÂNĂ"},    {"Ru", "РУССКИЙ"}, {"Sv", "SVENSKA"},  {"Tr", "TÜRKÇE"},    {"Zh", "简体中文"}};
    std::vector<Option> o;
    for (const auto& l : langs) o.push_back({l.first, l.second});
    m.choice(tr("TARGET LANGUAGE"), o,
             [c] {
               std::string v = conf_get(*c, "global.ai_target_lang");
               return v.empty() ? std::string("En") : v;
             },
             [c](const std::string& v) { conf_put(*c, "global.ai_target_lang", v); })
        .no_cycle();
    conf_text(m, cx, tr("AI TRANSLATION SERVICE URL"), "global.ai_service_url", false);
  };
  return make_menu(std::move(spec));
}

// ---- missing BIOS ------------------------------------------------------------------------------------------------------
struct BiosFile {
  std::string status, md5, path;
};
struct BiosSystem {
  std::string name;
  std::vector<BiosFile> files;
};
struct BiosState {
  bool done = false;
  std::string raw;  // written by the job, read after `done`
  std::vector<BiosSystem> systems;
};

/// Output of `rocknix-systems`: `> <system>` then `STATUS md5 path` lines (ES `getBiosInformations`).
std::vector<BiosSystem> parse_bios(const std::string& text) {
  std::vector<BiosSystem> out;
  size_t i = 0;
  while (i < text.size()) {
    size_t e = text.find('\n', i);
    std::string line = text.substr(i, e == std::string::npos ? std::string::npos : e - i);
    i = e == std::string::npos ? text.size() : e + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (starts_with(line, "> ")) {
      out.push_back({line.substr(2), {}});
      continue;
    }
    if (out.empty()) continue;
    size_t a = line.find(' ');
    if (a == std::string::npos) continue;
    size_t b = line.find(' ', a + 1);
    if (b == std::string::npos) continue;
    out.back().files.push_back({line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1)});
  }
  return out;
}

std::unique_ptr<Screen> make_bios() {
  auto st = std::make_shared<BiosState>();
  MenuSpec spec;
  spec.title = tr("MISSING BIOS CHECK");
  spec.title_key = N_("MISSING BIOS CHECK");
  spec.build = [st](MenuBuilder& m, Ctx&) {
    if (!st->done) {
      m.info(tr("PLEASE WAIT"), std::string());
      return;
    }
    bool any = false;
    for (const BiosSystem& s : st->systems) {
      if (s.files.empty()) continue;
      m.header(s.name);
      for (const BiosFile& f : s.files) {
        Entry& e = m.action(f.path, [] {}).value(tr(f.status.c_str()));
        if (!f.md5.empty() && f.md5 != "-") e.hint(std::string("MD5: ") + f.md5);
        any = true;
      }
    }
    if (!any) m.info(tr("NO MISSING BIOS FILES"), std::string());
  };
  spec.on_open = [st](Ctx& cx, const Refresh& refresh) {
    if (cx.sv->cmd.dry_run) {  // deterministic: tests and the preview harness do not run the script; a sample output can stand in
      std::string sample;
      if (read_file(env().rooted("/tmp/rocknix-systems.out"), sample)) st->systems = parse_bios(sample);
      st->done = true;
      if (refresh) refresh();
      return;
    }
    std::shared_ptr<Services> sv = cx.sv;
    cx.sv->jobs.post(
        [st, sv] { st->raw = sv->cmd.run("rocknix-systems", 120000).out; },
        [st, refresh] {
          st->systems = parse_bios(st->raw);
          st->done = true;
          if (refresh) refresh();
        });
  };
  return make_menu(std::move(spec));
}

}  // namespace

ZF_REGISTER_SCREEN("menu.game", [](Ctx&, const std::string&) { return make_game(); });
ZF_REGISTER_SCREEN("menu.game.systems", [](Ctx&, const std::string&) { return make_systems(); });
ZF_REGISTER_SCREEN("menu.game.ai", [](Ctx&, const std::string&) { return make_ai(); });
ZF_REGISTER_SCREEN("menu.game.bios", [](Ctx&, const std::string&) { return make_bios(); });

}  // namespace zf::mg
