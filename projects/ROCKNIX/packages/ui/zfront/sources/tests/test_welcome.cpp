// welcome.hpp: the pure logic of the first-boot welcome (steps, when it shows, languages, time zone step). Written before the code.
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "menu_welcome.hpp"
#include "welcome.hpp"

using namespace zf::welcome;

namespace {

// A fake zone database: offsets in seconds, no daylight saving (the real one is injected by the screen).
bool fake_offset(const std::string& zone, long long, int& off) {
  if (zone == "UTC") return off = 0, true;
  if (zone == "Europe/Madrid" || zone == "Europe/Paris") return off = 3600, true;
  if (zone == "Europe/Lisbon" || zone == "Europe/London") return off = 0, true;
  if (zone == "America/New_York") return off = -5 * 3600, true;
  if (zone == "Asia/Kolkata") return off = 5 * 3600 + 1800, true;
  if (zone == "Pacific/Kiritimati") return off = 14 * 3600, true;
  return false;
}

const std::vector<std::string> kZones = {"Africa/Abidjan", "America/Argentina/Buenos_Aires", "America/New_York", "Asia/Kolkata", "Etc/GMT+5",
                                         "Europe/Lisbon",  "Europe/London",  "Europe/Madrid", "Europe/Paris", "Pacific/Kiritimati", "UTC", "US/Eastern",
                                         "posix/Europe/Madrid"};

const std::vector<LangName> kKnown = {{"de_DE", "Deutsch"},   {"en_US", "English (US)"}, {"en_GB", "English (UK)"}, {"es_ES", "Español"},
                                      {"es_MX", "Español mexicano"}, {"fr_FR", "Français"}, {"pt_BR", "Português brasileiro"}};

Facts fresh() {
  Facts f;
  return f;
}

void test_flow_forward() {
  Flow f;
  CHECK_EQ(int(f.step()), int(Step::Language));
  CHECK_EQ(f.number(), 1);
  CHECK_EQ(f.total(), 4);
  f.apply(Ev::Next);
  CHECK_EQ(int(f.step()), int(Step::Wifi));
  f.apply(Ev::Next);
  CHECK_EQ(int(f.step()), int(Step::Zone));
  f.apply(Ev::Next);
  CHECK_EQ(int(f.step()), int(Step::Done));
  CHECK_EQ(f.number(), 4);
  CHECK(!f.finished());
  f.apply(Ev::Next);  // "Start playing"
  CHECK(f.finished());
  CHECK(!f.skipped());
}

void test_flow_skip_from_each_step() {
  for (int at = 0; at < 4; ++at) {
    Flow f;
    for (int i = 0; i < at; ++i) f.apply(Ev::Next);
    f.apply(Ev::Skip);  // Start
    CHECK(f.finished());
    CHECK(f.skipped());
  }
}

void test_flow_back_from_each_step() {
  Flow f;
  f.apply(Ev::Back);  // nothing before the first step
  CHECK_EQ(int(f.step()), int(Step::Language));
  CHECK(!f.finished());
  f.apply(Ev::Next), f.apply(Ev::Next), f.apply(Ev::Next);  // Done
  f.apply(Ev::Back);
  CHECK_EQ(int(f.step()), int(Step::Zone));
  f.apply(Ev::Back);
  CHECK_EQ(int(f.step()), int(Step::Wifi));
  f.apply(Ev::Back);
  CHECK_EQ(int(f.step()), int(Step::Language));
}

void test_flow_without_wifi() {
  Flow f(false);
  CHECK_EQ(f.total(), 3);
  f.apply(Ev::Next);
  CHECK_EQ(int(f.step()), int(Step::Zone));
  CHECK_EQ(f.number(), 2);
  f.apply(Ev::Next);
  CHECK_EQ(int(f.step()), int(Step::Done));
  CHECK_EQ(f.number(), 3);
  f.apply(Ev::Back), f.apply(Ev::Back);
  CHECK_EQ(int(f.step()), int(Step::Language));  // Back never lands on the missing WiFi step
}

void test_flow_ignores_events_when_finished() {
  Flow f;
  f.apply(Ev::Skip);
  f.apply(Ev::Next), f.apply(Ev::Back);
  CHECK(f.finished());
  CHECK(f.skipped());
  CHECK_EQ(int(f.step()), int(Step::Language));
}

void test_show_first_boot() {
  CHECK(should_show(fresh()));
}

void test_show_once() {
  Facts f = fresh();
  f.done = true;
  CHECK(!should_show(f));
}

void test_show_never_in_kid_or_kiosk() {
  Facts f = fresh();
  f.restricted = true;
  CHECK(!should_show(f));
  f.forced = true;  // "Run welcome again" does not bypass the lock either
  CHECK(!should_show(f));
}

void test_ota_over_used_console() {
  Facts f = fresh();
  f.wifi_saved = true;
  CHECK(!should_show(f));
  f = fresh();
  f.used_marker = true;
  CHECK(!should_show(f));
}

// A new console shows it whatever language and time zone it came with (es_ES + Europe/Madrid, zh_CN + Asia/Shanghai, a DEBUG image...): Facts
// does not even carry them. Only "done", a saved WiFi, the used marker and the restricted modes decide.
void test_any_language_and_zone_on_a_new_console() {
  CHECK(should_show(fresh()));
  const std::vector<LangName> offered = complete_languages({"es", "fr_FR"}, kKnown);
  CHECK_EQ(language_index(offered, "es_ES"), size_t(1));    // the cursor starts on the current language when it is offered
  CHECK_EQ(language_index(offered, "zh_CN"), size_t(0));    // not offered: English
  const std::vector<std::string> regs = regions(kZones);
  CHECK_EQ(initial_region("Europe/Madrid", "Europe/Madrid", regs), std::string("Europe"));
  CHECK_EQ(initial_region("Asia/Kolkata", "Asia/Kolkata", regs), std::string("Asia"));  // the current zone's region
  Facts f = fresh();
  f.wifi_saved = true;
  CHECK(!should_show(f));
  f = fresh();
  f.done = true;
  CHECK(!should_show(f));
}

void test_forced_ignores_done_and_use() {
  Facts f = fresh();
  f.done = true;
  f.wifi_saved = true;
  f.forced = true;
  CHECK(should_show(f));
}

void test_languages_only_complete() {
  auto l = complete_languages({"es", "fr_FR.UTF-8"}, kKnown);
  CHECK_EQ(l.size(), size_t(3));
  CHECK_EQ(l[0].code, std::string("en_US"));  // English always first, wherever it is in the table
  CHECK_EQ(l[1].code, std::string("es_ES"));  // bare "es" -> first known Spanish, not es_MX
  CHECK_EQ(l[2].code, std::string("fr_FR"));  // locale suffix stripped
}

void test_languages_exact_code_and_none() {
  auto l = complete_languages({"es_MX"}, kKnown);
  CHECK_EQ(l.size(), size_t(2));
  CHECK_EQ(l[1].code, std::string("es_MX"));
  auto e = complete_languages({}, kKnown);  // no catalog at all: still English
  CHECK_EQ(e.size(), size_t(1));
  CHECK_EQ(e[0].code, std::string("en_US"));
  auto n = complete_languages({"xx"}, kKnown);  // a catalog nobody can name is not offered
  CHECK_EQ(n.size(), size_t(1));
}

void test_language_index() {
  auto l = complete_languages({"es", "fr_FR"}, kKnown);
  CHECK_EQ(language_index(l, ""), size_t(0));
  CHECK_EQ(language_index(l, "en"), size_t(0));
  CHECK_EQ(language_index(l, "es"), size_t(1));
  CHECK_EQ(language_index(l, "es_ES"), size_t(1));
  CHECK_EQ(language_index(l, "fr_FR"), size_t(2));
  CHECK_EQ(language_index(l, "zz_ZZ"), size_t(0));
}

// The production path: the catalogs found on disk decide. A language that only ES translates (emulationstation2.mo) is not offered.
void test_language_needs_zfront_catalog_on_disk() {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("zf-welcome-lang-" + std::to_string(::getpid()));
  fs::remove_all(root);
  auto mo = [&](const char* lang, const char* domain) {
    fs::create_directories(root / lang / "LC_MESSAGES");
    std::ofstream(root / lang / "LC_MESSAGES" / (std::string(domain) + ".mo")) << "x";
  };
  mo("es", "zfront");
  mo("es", "emulationstation2");
  mo("pt_BR", "zfront");
  mo("ja_JP", "emulationstation2");  // ES has it, zfront does not
  mo("nl_NL", "emulationstation2");
  mo("zh_CN", "emulationstation2");
  zf::i18n::Options o;
  o.dirs = {root.string()};
  o.domains = {"zfront", "emulationstation2"};
  auto l = zf::welcome_languages(o);
  std::vector<std::string> codes;
  for (const LangName& n : l) codes.push_back(n.code);
  CHECK_EQ(codes.size(), size_t(3));
  CHECK(codes.size() == 3 && codes[0] == "en_US" && codes[1] == "es_ES" && codes[2] == "pt_BR");
  // a console whose language is not offered (zh_CN here) puts the cursor on English
  CHECK_EQ(language_index(l, "zh_CN"), size_t(0));
  CHECK_EQ(language_index(l, "ja_JP"), size_t(0));
  fs::remove_all(root);
}

void test_initial_region() {
  const std::vector<std::string> regs = regions(kZones);  // America, Asia, Europe, Pacific (Africa too)
  CHECK_EQ(initial_region("UTC", "", regs), std::string("Europe"));            // no clue, no zone: Europe
  CHECK_EQ(initial_region("UTC", "UTC", regs), std::string("Europe"));
  CHECK_EQ(initial_region("UTC", "Asia/Kolkata", regs), std::string("Asia"));  // the console already has a zone
  CHECK_EQ(initial_region("UTC", "Nowhere/City", regs), std::string("Europe"));
  CHECK_EQ(initial_region("America/New_York", "Europe/Madrid", regs), std::string("America"));  // a suggestion wins
  CHECK_EQ(initial_region("UTC", "", {"Africa", "Asia"}), std::string("Africa"));  // no Europe in the list: the first
  CHECK_EQ(initial_region("UTC", "", {}), std::string());
}

void test_clock_format_and_wrap() {
  const long long noon = 12 * 3600;  // 1970-01-01 12:00 UTC
  CHECK_EQ(format_clock(noon, 0), std::string("12:00"));
  CHECK_EQ(format_clock(noon, 3600), std::string("13:00"));
  CHECK_EQ(format_clock(noon, -5 * 3600), std::string("07:00"));
  CHECK_EQ(format_clock(noon, 5 * 3600 + 1800), std::string("17:30"));
  CHECK_EQ(format_clock(23 * 3600 + 30 * 60, 3600), std::string("00:30"));  // next day
  CHECK_EQ(format_clock(30 * 60, -3600), std::string("23:30"));             // previous day
  CHECK_EQ(format_clock(-3600LL, 0), std::string("23:00"));                 // before the epoch
  CHECK_EQ(format_clock(86400LL * 20000 + 9 * 3600 + 5 * 60, 0), std::string("09:05"));
}

void test_zone_clock_with_fake_clock() {
  const long long t = 10 * 3600 + 42 * 60;
  CHECK_EQ(zone_clock("Europe/Madrid", t, fake_offset), std::string("11:42"));
  CHECK_EQ(zone_clock("Europe/Lisbon", t, fake_offset), std::string("10:42"));
  CHECK_EQ(zone_clock("UTC", t, fake_offset), std::string("10:42"));
  CHECK_EQ(zone_clock("Pacific/Kiritimati", t, fake_offset), std::string("00:42"));
  CHECK_EQ(zone_clock("Mars/Olympus", t, fake_offset), std::string(""));  // invalid zone: no clock, no crash
}

void test_regions_filter_and_sort() {
  auto r = regions(kZones);
  std::vector<std::string> want = {"Africa", "America", "Asia", "Europe", "Pacific"};
  CHECK(r == want);  // Etc, US, posix, UTC dropped
}

void test_region_change_with_l_r() {
  auto r = regions(kZones);
  CHECK_EQ(step_region(r, "Europe", +1), std::string("Pacific"));
  CHECK_EQ(step_region(r, "Pacific", +1), std::string("Africa"));  // wraps
  CHECK_EQ(step_region(r, "Africa", -1), std::string("Pacific"));
  CHECK_EQ(step_region(r, "Europe", -1), std::string("Asia"));
  CHECK_EQ(step_region(r, "Nowhere", +1), std::string("Africa"));
  CHECK_EQ(step_region(r, "Nowhere", -1), std::string("Pacific"));
  CHECK_EQ(step_region({}, "Europe", +1), std::string(""));
}

void test_zones_in_region() {
  auto z = zones_in_region(kZones, "Europe");
  CHECK_EQ(z.size(), size_t(4));
  CHECK_EQ(z[0], std::string("Europe/Lisbon"));
  CHECK_EQ(zones_in_region(kZones, "America").size(), size_t(2));  // nested name stays in its region
  CHECK_EQ(zones_in_region(kZones, "Nowhere").size(), size_t(0));
}

void test_names() {
  CHECK_EQ(pretty_zone("Europe/Madrid"), std::string("Europe / Madrid"));
  CHECK_EQ(pretty_zone("America/Argentina/Buenos_Aires"), std::string("America / Argentina / Buenos Aires"));
  CHECK_EQ(pretty_zone("UTC"), std::string("UTC"));
  CHECK_EQ(region_of("Europe/Madrid"), std::string("Europe"));
  CHECK_EQ(region_of("UTC"), std::string(""));
}

void test_keep_utc_and_invalid_zone() {
  CHECK_EQ(resolve_zone(kZones, "UTC"), std::string("UTC"));  // "Keep UTC"
  CHECK_EQ(resolve_zone(kZones, ""), std::string("UTC"));
  CHECK_EQ(resolve_zone(kZones, "Europe/Madrid"), std::string("Europe/Madrid"));
  CHECK_EQ(resolve_zone(kZones, "Europe/Madriz"), std::string("UTC"));  // never written to system.timezone
  CHECK_EQ(resolve_zone({}, "Europe/Madrid"), std::string("UTC"));      // zone list failed to load: safe default
}

void test_suggestion_by_country_then_language() {
  CHECK_EQ(suggest_zone("ES", "en_US", kZones), std::string("Europe/Madrid"));  // the network country wins over the language
  CHECK_EQ(suggest_zone("", "es_ES", kZones), std::string("Europe/Madrid"));
  CHECK_EQ(suggest_zone("", "pt_PT", kZones), std::string("Europe/Lisbon"));
  CHECK_EQ(suggest_zone("GB", "", kZones), std::string("Europe/London"));
}

void test_suggestion_without_network_or_clue() {
  CHECK_EQ(suggest_zone("", "en_US", kZones), std::string("UTC"));  // English alone says nothing: Keep UTC
  CHECK_EQ(suggest_zone("", "", kZones), std::string("UTC"));
  CHECK_EQ(suggest_zone("ZZ", "xx", kZones), std::string("UTC"));
  CHECK_EQ(suggest_zone("ES", "", {"Europe/Paris"}), std::string("UTC"));  // suggested zone not installed: never invented
  CHECK_EQ(suggest_zone("ES", "", {}), std::string("UTC"));
}

}  // namespace

int main() {
  test_flow_forward();
  test_flow_skip_from_each_step();
  test_flow_back_from_each_step();
  test_flow_without_wifi();
  test_flow_ignores_events_when_finished();
  test_show_first_boot();
  test_show_once();
  test_show_never_in_kid_or_kiosk();
  test_ota_over_used_console();
  test_any_language_and_zone_on_a_new_console();
  test_forced_ignores_done_and_use();
  test_languages_only_complete();
  test_languages_exact_code_and_none();
  test_language_index();
  test_language_needs_zfront_catalog_on_disk();
  test_initial_region();
  test_clock_format_and_wrap();
  test_zone_clock_with_fake_clock();
  test_regions_filter_and_sort();
  test_region_change_with_l_r();
  test_zones_in_region();
  test_names();
  test_keep_utc_and_invalid_zone();
  test_suggestion_by_country_then_language();
  test_suggestion_without_network_or_clue();
  return zt::done();
}
