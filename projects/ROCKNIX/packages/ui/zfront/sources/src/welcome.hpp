// First-boot welcome: the PURE logic (no screen, no files, no clock of its own). Everything the screen needs to decide is here and
// is injected, so `tests/test_welcome.cpp` runs it on a PC with fake lists and a fake clock:
//   * `Flow`: the steps (language, WiFi, time zone, all set), Next / Back / Skip.
//   * `should_show`: when the welcome appears at all (once; never in Kid / Kiosk; never on a console that is already in use).
//   * `complete_languages`: the languages offered = English + those that have zfront's OWN catalog (list injected).
//   * the time zone step: regions (L / R), the zones of a region, the local time each zone would give, the suggested zone.
// The screen is `menu_welcome.cpp`. No allocation matters here: all of it runs on a key press or once a minute, never per frame.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace zf {
namespace welcome {

// ---- the steps --------------------------------------------------------------------------------------------------------------

enum class Step { Language = 0, Wifi = 1, Zone = 2, Done = 3 };
enum class Ev { Next, Back, Skip };

class Flow {
 public:
  /// `has_wifi` false: a console without a WiFi adapter goes Language -> Zone -> Done and counts "of 3".
  explicit Flow(bool has_wifi = true) : wifi_(has_wifi) {}

  Step step() const { return step_; }
  /// Position shown in the header, 1-based ("Welcome · 2 of 4").
  int number() const;
  int total() const { return wifi_ ? 4 : 3; }
  /// The flow ended, by the last "Start playing" or by Start (skip all). Events after that do nothing.
  bool finished() const { return finished_; }
  bool skipped() const { return skipped_; }
  /// Next: moves on; on the last step it finishes. Back: one step back (nothing on the first). Skip: finishes at once.
  void apply(Ev e);

 private:
  bool wifi_;
  Step step_ = Step::Language;
  bool finished_ = false, skipped_ = false;
};

// ---- when to show it --------------------------------------------------------------------------------------------------------

struct Facts {
  bool done = false;        // `zfront.welcome_done` is set
  bool restricted = false;  // UIMode is Kid or Kiosk
  bool forced = false;      // "Run welcome again" (System): ignores `done` and the signs of use, never the restriction
  bool wifi_saved = false;  // wifi.ssid is set: somebody already connected this console
  bool used_marker = false; // anything else that says the console has been set up (decided by the caller)
};
/// The welcome appears on EVERY first boot of a new console, whatever its language or time zone (a DEBUG image comes with Madrid already
/// set): when it was never done, the mode is Full and nothing says the console is already in use (a saved WiFi network or `used_marker`).
/// An OTA over a used console therefore never shows it.
bool should_show(const Facts& f);

// ---- languages --------------------------------------------------------------------------------------------------------------

struct LangName {
  std::string code, name;
};
/// `catalogs`: language codes that have zfront's own catalog ("es", "es_ES", "es_ES.UTF-8"...). `known`: the code/name table of the
/// language menu, in its order. Result: English (`en_US`) first, then every known language that has a catalog. An exact code wins; a catalog
/// with the bare language ("es") gives the FIRST known code of that language ("es_ES", not "es_MX").
std::vector<LangName> complete_languages(const std::vector<std::string>& catalogs, const std::vector<LangName>& known);
/// Index in `langs` of the stored `system.language` ("" or "en" = English, "es" matches "es_ES"); 0 when nothing matches.
size_t language_index(const std::vector<LangName>& langs, const std::string& stored);

// ---- time zones -------------------------------------------------------------------------------------------------------------

/// Offset from UTC, in seconds, of `zone` at the instant `utc` (seconds since the epoch). False: unknown zone.
using OffsetFn = std::function<bool(const std::string& zone, long long utc, int& offset_s)>;

/// "HH:MM" (24 h) of the instant `utc` shifted by `offset_s`; negative and day-crossing results wrap correctly.
std::string format_clock(long long utc, int offset_s);
/// The clock a zone would show right now, or "" when the zone is unknown to `off`.
std::string zone_clock(const std::string& zone, long long utc, const OffsetFn& off);

/// Regions (the part before the first "/") of the zones, sorted, only the continents a person would pick ("Etc", "posix", "US"... dropped).
std::vector<std::string> regions(const std::vector<std::string>& zones);
/// Zones of one region, in the order given.
std::vector<std::string> zones_in_region(const std::vector<std::string>& zones, const std::string& region);
/// Region after / before `current` (dir = +1 / -1), wrapping around. `current` not in the list: the first (or last) one.
std::string step_region(const std::vector<std::string>& regs, const std::string& current, int dir);
/// Region of `zone` ("Europe/Madrid" -> "Europe"; "UTC" -> "").
std::string region_of(const std::string& zone);
/// "Europe/Madrid" -> "Europe / Madrid", underscores as spaces ("America / Argentina / Buenos Aires").
std::string pretty_zone(const std::string& zone);

/// The zone to store for the chosen one: itself when it is in `zones`, "UTC" for "UTC" / "" / anything unknown (an invalid zone never
/// reaches `system.timezone`).
std::string resolve_zone(const std::vector<std::string>& zones, const std::string& wanted);

/// The region the zone step opens on. With a suggestion (`pick` != "UTC") it is the region of `pick`. Without one: the region of the zone the
/// console has now (`current_zone`), or "Europe" when that is UTC / unknown. Always one of `regs` (else the first, or "" when there are none).
std::string initial_region(const std::string& pick, const std::string& current_zone, const std::vector<std::string>& regs);

/// Where to put the cursor. There is NO external service: only what the console already knows. In this order: (1) `wifi.country`
/// (ISO code, the console's own setting) through a small table; (2) the language, but only for languages that mean one zone (es_ES, pt_PT,
/// de, fr, it, en_GB, ja, ko...; NOT en_US, which spans four); (3) "UTC" (the "Keep UTC" row). The result is always in `zones` or "UTC".
std::string suggest_zone(const std::string& country, const std::string& language, const std::vector<std::string>& zones);

}  // namespace welcome
}  // namespace zf
