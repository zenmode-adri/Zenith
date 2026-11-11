#include "scraper_src.hpp"

#include "scraper_adb.hpp"
#include "scraper_igdb.hpp"
#include "scraper_ss.hpp"
#include "util.hpp"

namespace zf::scraper {

const char* source_name(Source s) {
  switch (s) {
    case Source::IGDB: return "IGDB";
    case Source::ArcadeDB: return "ArcadeDB";
    default: return "ScreenScraper";
  }
}

Source source_from_name(std::string_view name) {
  if (name == "IGDB") return Source::IGDB;
  if (name == "ArcadeDB") return Source::ArcadeDB;
  return Source::ScreenScraper;
}

Source source_of(const EsSettings& es) {
  const std::string* v = es.get(kKeySource);
  return v ? source_from_name(*v) : Source::ScreenScraper;
}

std::vector<Source> sources() { return {Source::ScreenScraper, Source::IGDB, Source::ArcadeDB}; }

bool available(Source s) { return s != Source::ScreenScraper || !ss::dev_login().empty(); }

bool platform_supported(Source s, std::string_view platforms, std::string_view system_name) {
  switch (s) {
    case Source::IGDB: return igdb::platform_supported(platforms);
    case Source::ArcadeDB: return adb::platform_supported(platforms, system_name);
    default: return ss::platform_supported(platforms);
  }
}

std::string search_stem(std::string_view rom_file) {
  std::string stem = path_stem(rom_file), o;
  int depth_p = 0, depth_b = 0;
  for (char c : stem) {
    if (c == '(') ++depth_p;
    else if (c == ')') {
      if (depth_p) --depth_p;
    } else if (c == '[') ++depth_b;
    else if (c == ']') {
      if (depth_b) --depth_b;
    } else if (!depth_p && !depth_b) o.push_back(c);
  }
  return std::string(trim(o));
}

}  // namespace zf::scraper
