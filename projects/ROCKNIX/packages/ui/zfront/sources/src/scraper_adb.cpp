#include "scraper_adb.hpp"

#include <cctype>

#include "json.hpp"

namespace zf::scraper::adb {
namespace {

bool has_word(std::string_view s, std::string_view w) {
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i;
    size_t b = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\n') ++i;
    if (i > b && s.substr(b, i - b) == w) return true;
  }
  return false;
}

// ES getMediaTagNames(): the ArcadeDB fields that hold each kind of picture, in order of preference
std::vector<const char*> media_tags(const std::string& src) {
  if (src == "ss" || src == "mixrbv2" || src == "mixrbv1" || src == "mixrbv") return {"url_image_ingame"};
  if (src == "sstitle") return {"url_image_title"};
  if (src == "box-2D" || src == "box-3D") return {"url_image_flyer"};
  if (src == "wheel" || src == "wheel-hd" || src == "marquee") return {"url_image_marquee"};
  if (src == "video") return {"url_video_shortplay", "url_video_shortplay_hd"};
  return {};
}

std::string find_media(const json::Value& g, const std::string& src) {
  for (const char* tag : media_tags(src)) {
    const json::Value* v = g.find(tag);
    if (v && v->type == json::Value::Type::String && !v->str.empty()) return v->str;
  }
  return std::string();
}

std::string str_of(const json::Value& g, const char* key) {
  const json::Value* v = g.find(key);
  return v && v->type == json::Value::Type::String ? v->str : std::string();
}

}  // namespace

bool platform_supported(std::string_view platforms, std::string_view system_name) {
  if (system_name == "teknoparrot") return false;
  return has_word(platforms, "arcade") || has_word(platforms, "neogeo") || has_word(platforms, "lcdgames");
}

std::string game_name(std::string_view rom_file) {
  std::string n = search_stem(rom_file), o;
  for (char c : n)
    if (c != ' ' && c != '.') o.push_back(c);
  return o;
}

HttpRequest game_request(std::string_view rom_file) {
  HttpRequest r;
  r.url = "https://adb.arcadeitalia.net/service_scraper.php?ajax=query_mame&lang=en&use_parent=1&game_name=" + ss::url_encode(game_name(rom_file));
  return r;
}

bool valid_answer(std::string_view text) {
  json::Value v;
  if (!json::parse(text, v) || !v.is_object() || v.integer("release", 0) < 4) return false;
  const json::Value* res = v.find("result");
  return res && res->is_array();
}

bool parse_games(std::string_view text, const ss::Wish& w, bool short_title, ss::Parsed& out) {
  json::Value v;
  if (!json::parse(text, v) || !v.is_object()) return false;
  if (v.integer("release", 0) < 4) return false;  // ES ARCADE_DB_RELEASE_VERSION
  const json::Value* res = v.find("result");
  if (!res || !res->is_array()) return false;
  for (const json::Value& g : res->arr) {
    if (!g.is_object()) continue;
    ss::Parsed p;
    std::string title = str_of(g, "title");
    if (short_title) {
      std::string st = str_of(g, "short_title");
      if (!st.empty()) title = st;
    }
    if (title.empty()) continue;  // ES: getStringOrThrow("title")
    p.name = title;
    p.desc = str_of(g, "history");
    std::string year = str_of(g, "year");
    bool digits = year.size() == 4;
    for (char c : year) digits = digits && std::isdigit(static_cast<unsigned char>(c));
    if (digits) p.date = year + "0101T000000";
    std::string maker = str_of(g, "manufacturer");
    p.developer = maker;
    p.publisher = maker;
    p.genre = str_of(g, "genre");
    if (const json::Value* pl = g.find("players"); pl && pl->type == json::Value::Type::Number) p.players = g.text("players");

    std::string art = find_media(g, w.image_src);
    if (!art.empty()) p.picks.push_back({"image", art, ".png"});
    if (!w.thumb_src.empty() && w.thumb_src != w.image_src) {
      art = find_media(g, w.thumb_src);
      if (!art.empty()) p.picks.push_back({"thumb", art, ".png"});
    }
    if (!w.logo_src.empty()) {
      art = find_media(g, w.logo_src);
      if (!art.empty()) p.picks.push_back({"marquee", art, ".png"});
    }
    if (w.titleshot) {
      art = find_media(g, "sstitle");
      if (!art.empty()) p.picks.push_back({"titleshot", art, ".png"});
    }
    if (w.video) {
      art = find_media(g, "video");
      if (!art.empty()) p.picks.push_back({"video", art, ""});
    }
    out = std::move(p);
    return true;
  }
  return false;
}

}  // namespace zf::scraper::adb
