// See welcome.hpp. Pure logic: no I/O, no clock, no allocation per frame (nothing here is called per frame).
#include "welcome.hpp"

#include <algorithm>
#include <cstdio>

namespace zf {
namespace welcome {

namespace {

// ---- small string helpers (no dependency on the rest of zfront: this file is built into the test on its own)

std::string strip_locale(const std::string& s) {  // "es_ES.UTF-8@euro" -> "es_ES"
  size_t cut = s.find_first_of(".@");
  return cut == std::string::npos ? s : s.substr(0, cut);
}

std::string base_of(const std::string& code) {  // "es_ES" -> "es"
  size_t u = code.find('_');
  return u == std::string::npos ? code : code.substr(0, u);
}

bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

// The continents a person picks from; everything else in the zone database ("Etc", "US", "posix", "SystemV"...) is a duplicate or a
// technical alias and would only make the list longer.
bool is_region(const std::string& r) {
  static const char* const kRegions[] = {"Africa", "America", "Antarctica", "Arctic", "Asia", "Atlantic", "Australia", "Europe", "Indian", "Pacific"};
  for (const char* k : kRegions)
    if (r == k) return true;
  return false;
}

struct Pair {
  const char* key;
  const char* zone;
};

// wifi.country (ISO 3166-1) -> the zone of the country's capital area; only countries with one zone, or one that holds most people.
const Pair kCountryZone[] = {
    {"ES", "Europe/Madrid"},  {"PT", "Europe/Lisbon"},  {"GB", "Europe/London"},   {"IE", "Europe/Dublin"},   {"FR", "Europe/Paris"},
    {"DE", "Europe/Berlin"},  {"IT", "Europe/Rome"},    {"NL", "Europe/Amsterdam"}, {"BE", "Europe/Brussels"}, {"AT", "Europe/Vienna"},
    {"CH", "Europe/Zurich"},  {"PL", "Europe/Warsaw"},  {"SE", "Europe/Stockholm"}, {"NO", "Europe/Oslo"},     {"DK", "Europe/Copenhagen"},
    {"FI", "Europe/Helsinki"}, {"GR", "Europe/Athens"}, {"TR", "Europe/Istanbul"}, {"MX", "America/Mexico_City"}, {"AR", "America/Argentina/Buenos_Aires"},
    {"CO", "America/Bogota"}, {"PE", "America/Lima"},   {"CL", "America/Santiago"}, {"BR", "America/Sao_Paulo"}, {"JP", "Asia/Tokyo"},
    {"KR", "Asia/Seoul"},     {"IN", "Asia/Kolkata"},   {"NZ", "Pacific/Auckland"},
};

// Language -> zone only where the language means one zone. English (US) and Portuguese (BR/bare) are left out on purpose.
const Pair kLanguageZone[] = {
    {"es_ES", "Europe/Madrid"}, {"es", "Europe/Madrid"}, {"pt_PT", "Europe/Lisbon"}, {"en_GB", "Europe/London"}, {"fr", "Europe/Paris"},
    {"de", "Europe/Berlin"},    {"it", "Europe/Rome"},   {"ja", "Asia/Tokyo"},       {"ko", "Asia/Seoul"},       {"es_MX", "America/Mexico_City"},
    {"pt_BR", "America/Sao_Paulo"},
};

const char* lookup(const Pair* t, size_t n, const std::string& k) {
  for (size_t i = 0; i < n; ++i)
    if (k == t[i].key) return t[i].zone;
  return nullptr;
}

bool has_zone(const std::vector<std::string>& zones, const std::string& z) { return std::find(zones.begin(), zones.end(), z) != zones.end(); }

}  // namespace

// ---- Flow -------------------------------------------------------------------------------------------------------------------

int Flow::number() const {
  int n = int(step_) + 1;
  if (!wifi_ && step_ != Step::Language) --n;  // the WiFi step does not exist: Zone is 2, Done is 3
  return n;
}

void Flow::apply(Ev e) {
  if (finished_) return;
  switch (e) {
    case Ev::Skip:
      finished_ = skipped_ = true;
      return;
    case Ev::Next:
      switch (step_) {
        case Step::Language: step_ = wifi_ ? Step::Wifi : Step::Zone; break;
        case Step::Wifi: step_ = Step::Zone; break;
        case Step::Zone: step_ = Step::Done; break;
        case Step::Done: finished_ = true; break;
      }
      return;
    case Ev::Back:
      switch (step_) {
        case Step::Language: break;
        case Step::Wifi: step_ = Step::Language; break;
        case Step::Zone: step_ = wifi_ ? Step::Wifi : Step::Language; break;
        case Step::Done: step_ = Step::Zone; break;
      }
      return;
  }
}

// ---- when to show it --------------------------------------------------------------------------------------------------------

bool should_show(const Facts& f) {
  if (f.restricted) return false;
  if (f.forced) return true;
  if (f.done) return false;
  if (f.wifi_saved || f.used_marker) return false;
  return true;
}

// ---- languages --------------------------------------------------------------------------------------------------------------

std::vector<LangName> complete_languages(const std::vector<std::string>& catalogs, const std::vector<LangName>& known) {
  std::vector<std::string> have;
  for (const std::string& c : catalogs) have.push_back(strip_locale(c));
  auto exact = [&](const std::string& code) { return std::find(have.begin(), have.end(), code) != have.end(); };

  std::vector<LangName> out;
  for (const LangName& k : known)
    if (k.code == "en_US") out.push_back(k);
  if (out.empty()) out.push_back({"en_US", "English"});

  for (const LangName& k : known) {
    if (k.code == "en_US") continue;
    bool take = exact(k.code);
    if (!take && exact(base_of(k.code))) {  // a bare "es" catalog: only the first known code of that language
      take = true;
      for (const LangName& o : known) {
        if (&o == &k) break;
        if (base_of(o.code) == base_of(k.code) && !exact(o.code)) {  // an earlier sibling already took the bare catalog
          take = false;
          break;
        }
      }
    }
    if (take) out.push_back(k);
  }
  return out;
}

size_t language_index(const std::vector<LangName>& langs, const std::string& stored) {
  const std::string s = strip_locale(stored);
  if (s.empty() || s == "en") return 0;
  for (size_t i = 0; i < langs.size(); ++i)
    if (langs[i].code == s) return i;
  for (size_t i = 0; i < langs.size(); ++i)
    if (starts_with(langs[i].code, s + "_")) return i;  // "es" -> "es_ES"
  return 0;
}

// ---- time zones -------------------------------------------------------------------------------------------------------------

std::string format_clock(long long utc, int offset_s) {
  long long t = utc + offset_s;
  long long day = 86400;
  long long sod = ((t % day) + day) % day;  // seconds of the day, also for instants before the epoch
  char buf[8];
  std::snprintf(buf, sizeof buf, "%02d:%02d", int(sod / 3600), int((sod % 3600) / 60));
  return buf;
}

std::string zone_clock(const std::string& zone, long long utc, const OffsetFn& off) {
  int o = 0;
  if (!off || !off(zone, utc, o)) return {};
  return format_clock(utc, o);
}

std::string region_of(const std::string& zone) {
  size_t s = zone.find('/');
  return s == std::string::npos ? std::string() : zone.substr(0, s);
}

std::vector<std::string> regions(const std::vector<std::string>& zones) {
  std::vector<std::string> out;
  for (const std::string& z : zones) {
    std::string r = region_of(z);
    if (is_region(r) && std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> zones_in_region(const std::vector<std::string>& zones, const std::string& region) {
  std::vector<std::string> out;
  for (const std::string& z : zones)
    if (region_of(z) == region) out.push_back(z);
  return out;
}

std::string step_region(const std::vector<std::string>& regs, const std::string& current, int dir) {
  if (regs.empty()) return {};
  auto it = std::find(regs.begin(), regs.end(), current);
  if (it == regs.end()) return dir >= 0 ? regs.front() : regs.back();
  size_t i = size_t(it - regs.begin());
  i = dir >= 0 ? (i + 1) % regs.size() : (i + regs.size() - 1) % regs.size();
  return regs[i];
}

std::string pretty_zone(const std::string& zone) {
  std::string out;
  for (char c : zone) {
    if (c == '/') out += " / ";
    else out += c == '_' ? ' ' : c;
  }
  return out;
}

std::string resolve_zone(const std::vector<std::string>& zones, const std::string& wanted) {
  if (wanted.empty() || wanted == "UTC") return "UTC";
  return has_zone(zones, wanted) ? wanted : std::string("UTC");
}

std::string initial_region(const std::string& pick, const std::string& current_zone, const std::vector<std::string>& regs) {
  auto has = [&](const std::string& r) { return !r.empty() && std::find(regs.begin(), regs.end(), r) != regs.end(); };
  std::string r = pick == "UTC" ? region_of(current_zone) : region_of(pick);
  if (!has(r) && pick == "UTC") r = "Europe";
  if (!has(r)) r = regs.empty() ? std::string() : regs.front();
  return r;
}

std::string suggest_zone(const std::string& country, const std::string& language, const std::vector<std::string>& zones) {
  if (const char* z = lookup(kCountryZone, sizeof kCountryZone / sizeof *kCountryZone, country))
    if (has_zone(zones, z)) return z;
  const std::string lang = strip_locale(language);
  if (!lang.empty()) {
    const char* z = lookup(kLanguageZone, sizeof kLanguageZone / sizeof *kLanguageZone, lang);
    if (!z) z = lookup(kLanguageZone, sizeof kLanguageZone / sizeof *kLanguageZone, base_of(lang));
    if (z && has_zone(zones, z)) return z;
  }
  return "UTC";
}

}  // namespace welcome
}  // namespace zf
