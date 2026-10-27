// Group screens (see menu_groups.hpp): each one is made of sections that already exist.
#include "menu_groups.hpp"

#include <memory>

#include "menu_lang.hpp"
#include "menu_ui.hpp"
#include "registry.hpp"
#include "util.hpp"

namespace zf {

MenuSpec merge_specs(std::string title, const char* title_key, std::vector<MenuSpec> parts) {
  auto all = std::make_shared<std::vector<MenuSpec>>(std::move(parts));
  MenuSpec s;
  s.title = std::move(title);
  s.title_key = title_key;
  s.build = [all](MenuBuilder& m, Ctx& cx) {
    for (const MenuSpec& p : *all)
      if (p.build) p.build(m, cx);
  };
  s.on_open = [all](Ctx& cx, const Refresh& r) {
    for (const MenuSpec& p : *all)
      if (p.on_open) p.on_open(cx, r);
  };
  s.on_close = [all](Ctx& cx) {
    for (const MenuSpec& p : *all)
      if (p.on_close) p.on_close(cx);
  };
  return s;
}

namespace {

// A part of a group that is only a list of links to other screens.
MenuSpec links(std::function<void(MenuBuilder&, Ctx&)> f) {
  MenuSpec s;
  s.build = [f = std::move(f)](MenuBuilder& m, Ctx& cx) { f(m, cx); };
  return s;
}

std::unique_ptr<Screen> group(MenuSpec s) { return std::make_unique<LangMenu>(std::move(s)); }

/// A screen with its own title made of one section (the sub-screens of a group).
MenuSpec titled(MenuSpec s, std::string title, const char* key) {
  s.title = std::move(title);
  s.title_key = key;
  return s;
}

/// The language's own name for what system.cfg stores ("es" and "es_ES" are both Spanish), for the value of a row.
std::string language_label(Ctx& cx) {
  const std::string stored = cx.conf.get_or("system.language", "");
  if (stored.empty()) return "English (US)";
  for (const LanguageName& l : language_names())
    if (stored == l.code) return l.name;
  for (const LanguageName& l : language_names())
    if (starts_with(l.code, stored + "_")) return l.name;
  return stored;
}

}  // namespace

// SCREEN & SOUND: what you see and hear. Brightness (+ panel calibration one level down), audio output, LEDs, top bar.
// The volume itself is the hardware buttons' job: only its on-screen overlay is a setting here.
ZF_REGISTER_SCREEN("menu.g.screen", [](Ctx& cx, const std::string&) {
  using namespace sys_sec;
  std::vector<MenuSpec> parts;
  parts.push_back(system_spec(cx, kBright | kNoHdr));  // no header: the title says it, and the screen is one row too long with it
  if (system_has_section(kPanel)) parts.push_back(links([](MenuBuilder& m, Ctx&) { m.submenu(tr("PANEL ADJUSTMENT"), "menu.g.panel"); }));
  parts.push_back(sound_spec(cx, snd_part::kAudio | snd_part::kOverlay));
  parts.push_back(system_spec(cx, kLeds));
  parts.push_back(ui_spec(ui_part::kTopBar));
  return group(merge_specs(tr("SCREEN & SOUND"), N_("SCREEN & SOUND"), std::move(parts)));
});
// Gamma, contrast, saturation, hue: calibration, touched once.
ZF_REGISTER_SCREEN("menu.g.panel", [](Ctx& cx, const std::string&) {
  return group(titled(system_spec(cx, sys_sec::kPanel), tr("PANEL ADJUSTMENT"), N_("PANEL ADJUSTMENT")));
});

// BATTERY & PERFORMANCE: what the console does with its power. Governors and cooling first, then what happens when it is
// idle (suspend, screensaver), then the battery warning; overclock one level down.
ZF_REGISTER_SCREEN("menu.g.power", [](Ctx& cx, const std::string&) {
  using namespace sys_sec;
  std::vector<MenuSpec> parts;
  parts.push_back(system_spec(cx, kPower));
  parts.push_back(links([](MenuBuilder& m, Ctx&) { m.header(tr("IDLE")); }));
  parts.push_back(system_spec(cx, kSuspend | kNoHdr));
  parts.push_back(ui_spec(ui_part::kSaver | ui_part::kNoHdr));
  parts.push_back(sound_spec(cx, snd_part::kBatt));
  if (system_has_section(kTurbo)) parts.push_back(links([](MenuBuilder& m, Ctx&) { m.submenu(tr("ADVANCED"), "menu.g.power.adv"); }));
  return group(merge_specs(tr("BATTERY & PERFORMANCE"), N_("BATTERY & PERFORMANCE"), std::move(parts)));
});
ZF_REGISTER_SCREEN("menu.g.power.adv", [](Ctx& cx, const std::string&) {
  return group(titled(system_spec(cx, sys_sec::kTurbo | sys_sec::kNoHdr), tr("ADVANCED"), N_("ADVANCED")));
});

// LIBRARY: your games. Collections, scraper, the card they live on.
ZF_REGISTER_SCREEN("menu.g.library", [](Ctx& cx, const std::string&) {
  return group(merge_specs(tr("LIBRARY"), N_("LIBRARY"),
                           {links([](MenuBuilder& m, Ctx&) {
                              m.submenu(tr("GAME COLLECTION SETTINGS"), "menu.collections");
                              m.submenu(tr("SCRAPER"), "menu.scraper");
                            }),
                            system_spec(cx, sys_sec::kStorage)}));
});

// SYSTEM: the console itself. Information, language and time, updates, maintenance; the rare things one level down.
ZF_REGISTER_SCREEN("menu.g.system", [](Ctx& cx, const std::string&) {
  using namespace sys_sec;
  std::vector<MenuSpec> parts;
  parts.push_back(system_spec(cx, kInfo | kNoHdr));
  parts.push_back(links([](MenuBuilder& m, Ctx& cx) { m.submenu(tr("LANGUAGE & TIME"), "menu.g.lang").value(language_label(cx)); }));
  parts.push_back(links([](MenuBuilder& m, Ctx&) { m.submenu(tr("UPDATES & DOWNLOADS"), "menu.updates"); }));
  parts.push_back(system_spec(cx, kReset | kNoHdr));
  if (system_has_section(kSysAdv)) parts.push_back(links([](MenuBuilder& m, Ctx&) { m.submenu(tr("ADVANCED"), "menu.g.system.adv"); }));
  return group(merge_specs(tr("SYSTEM"), N_("SYSTEM"), std::move(parts)));
});
// Language, keyboard layout and variant, timezone: four rows nobody touches twice.
ZF_REGISTER_SCREEN("menu.g.lang", [](Ctx& cx, const std::string&) {
  return group(titled(system_spec(cx, sys_sec::kLang | sys_sec::kNoHdr), tr("LANGUAGE & TIME"), N_("LANGUAGE & TIME")));
});
ZF_REGISTER_SCREEN("menu.g.system.adv", [](Ctx& cx, const std::string&) {
  return group(titled(system_spec(cx, sys_sec::kSysAdv | sys_sec::kNoHdr), tr("ADVANCED"), N_("ADVANCED")));
});

}  // namespace zf
