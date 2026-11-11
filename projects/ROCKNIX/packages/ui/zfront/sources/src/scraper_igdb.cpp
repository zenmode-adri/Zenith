#include "scraper_igdb.hpp"

#include <cstring>

#include "json.hpp"

namespace zf::scraper::igdb {
namespace {

struct Plat {
  const char* name;
  const char* ids;  // comma separated
};
// ES platform name -> IGDB platform ids: igdb_platformids of IGDBScraper.cpp:23-120 through PlatformId.cpp's names (sorted by name)
const Plat kPlat[] = {
    {"3do", "50"},
    {"3ds", "37"},
    {"adam", "68"},
    {"amiga", "16"},
    {"amigacd32", "114"},
    {"amigacdtv", "158"},
    {"amstradcpc", "25"},
    {"apple2", "75"},
    {"apple2gs", "115"},
    {"arcade", "52"},
    {"archimedes", "116"},
    {"astrocade", "91"},
    {"astrocde", "91"},
    {"atari2600", "59"},
    {"atari5200", "66"},
    {"atari7800", "60"},
    {"atari800", "89769"},
    {"atarijaguar", "62"},
    {"atarijaguarcd", "410"},
    {"atarilynx", "61"},
    {"atarist", "63"},
    {"atom", "116"},
    {"atomiswave", "52"},
    {"bbc", "69"},
    {"bbcmicro", "69"},
    {"c20", "71"},
    {"c64", "15"},
    {"cdi", "117"},
    {"channelf", "127"},
    {"chihiro", "52"},
    {"colecovision", "68"},
    {"cpet", "90"},
    {"cplus4", "94"},
    {"dreamcast", "23"},
    {"electron", "134"},
    {"fm7", "152"},
    {"fmtowns", "118"},
    {"gameandwatch", "307"},
    {"gamegear", "35"},
    {"gb", "33"},
    {"gb2players", "33"},
    {"gba", "24"},
    {"gbc", "22"},
    {"gbc2players", "22"},
    {"gc", "21"},
    {"genesis", "29"},
    {"gx4000", "25"},
    {"intellivision", "67"},
    {"jaguar", "62"},
    {"mastersystem", "64"},
    {"megacd", "78"},
    {"moonlight", "13,6"},
    {"msx", "27"},
    {"multivision", "84"},
    {"n3ds", "37"},
    {"n64", "4"},
    {"n64dd", "4"},
    {"nds", "20"},
    {"neogeo", "79"},
    {"neogeocd", "136"},
    {"nes", "18"},
    {"ngp", "119"},
    {"ngpc", "120"},
    {"o2em", "133"},
    {"odyssey2", "133"},
    {"pc", "13"},
    {"pc88", "125"},
    {"pc98", "149"},
    {"pcarcade", "52"},
    {"pcengine", "86"},
    {"pcenginecd", "150"},
    {"pcfx", "274"},
    {"pet", "90"},
    {"pico", "339"},
    {"ps2", "8"},
    {"ps3", "9"},
    {"psp", "38"},
    {"psvita", "46"},
    {"psx", "131"},
    {"satellaview", "306"},
    {"saturn", "32"},
    {"scummvm", "6"},
    {"scv", "376"},
    {"sega32x", "30"},
    {"segacd", "78"},
    {"sg-1000", "84"},
    {"sg1000", "84"},
    {"snes", "19"},
    {"snes-msu1", "19"},
    {"supergrafx", "128"},
    {"supervision", "415"},
    {"switch", "130"},
    {"teknoparrot", "52"},
    {"thomson", "156"},
    {"ti99", "129"},
    {"ti99_4a", "129"},
    {"trs-80", "126"},
    {"vectrex", "70"},
    {"vic20", "71"},
    {"videopac", "133"},
    {"virtualboy", "87"},
    {"watara", "415"},
    {"wii", "5"},
    {"wiiu", "41"},
    {"windows", "13,6"},
    {"wonderswan", "57"},
    {"wonderswancolor", "123"},
    {"x1", "77"},
    {"x68000", "121"},
    {"xbox", "11"},
    {"xbox360", "12"},
    {"xegs", "65"},
    {"zx81", "373"},
    {"zxspectrum", "2"},
};

std::vector<std::string_view> words(std::string_view s) {
  std::vector<std::string_view> w;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n')) ++i;
    size_t b = i;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t' && s[i] != '\n') ++i;
    if (i > b) w.push_back(s.substr(b, i - b));
  }
  return w;
}

// apicalypse string literal: backslash and quote escaped, control characters dropped
std::string quote(std::string_view s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o.push_back('\\');
    if (static_cast<unsigned char>(c) < 0x20) continue;
    o.push_back(c);
  }
  return o + "\"";
}

const char* kImg = "https://images.igdb.com/igdb/image/upload/";

std::string image_id(const json::Value& v) {
  if (!v.is_object()) return std::string();
  const json::Value* id = v.find("image_id");
  return id && id->type == json::Value::Type::String ? id->str : std::string();
}

}  // namespace

std::vector<std::string> platform_ids(std::string_view platforms) {
  std::vector<std::string> out;
  for (std::string_view w : words(platforms))
    for (const Plat& p : kPlat) {
      if (w != p.name) continue;
      std::string_view ids = p.ids;
      size_t b = 0;
      while (b <= ids.size()) {
        size_t c = ids.find(',', b);
        if (c == std::string_view::npos) c = ids.size();
        std::string id(ids.substr(b, c - b));
        bool seen = false;
        for (const std::string& o : out) seen = seen || o == id;
        if (!id.empty() && !seen) out.push_back(id);
        b = c + 1;
      }
    }
  return out;
}

bool platform_supported(std::string_view platforms) { return !platform_ids(platforms).empty(); }

HttpRequest token_request(std::string_view client_id, std::string_view client_secret) {
  HttpRequest r;
  r.url = "https://id.twitch.tv/oauth2/token";
  r.headers.push_back("Content-Type: application/x-www-form-urlencoded");
  r.body = "client_id=" + ss::url_encode(client_id) + "&client_secret=" + ss::url_encode(client_secret) + "&grant_type=client_credentials";
  r.post = true;
  return r;
}

bool parse_token(std::string_view text, std::string& token, long& expires_in) {
  json::Value v;
  token.clear();
  expires_in = 0;
  if (!json::parse(text, v) || !v.is_object()) return false;
  const json::Value* t = v.find("access_token");
  if (!t || t->type != json::Value::Type::String || t->str.empty()) return false;
  token = t->str;
  expires_in = v.integer("expires_in", 0);
  return true;
}

long token_ttl_s(long expires_in) { return expires_in > 0 && expires_in < 1800 ? expires_in : 1800; }

HttpRequest game_request(std::string_view client_id, std::string_view token, std::string_view name, std::string_view platform_id) {
  HttpRequest r;
  r.url = "https://api.igdb.com/v4/games";
  r.headers.push_back("Client-ID: " + std::string(client_id));
  r.headers.push_back("Authorization: Bearer " + std::string(token));
  r.headers.push_back("Content-Type: text/plain");  // ES sends the misspelt "ContentType": the API takes the body as text either way
  r.headers.push_back("Accept: application/json");
  r.body =
      "fields id, name, platforms.name, genres.name, game_modes.name, multiplayer_modes.offlinemax, release_dates.date, "
      "release_dates.region, release_dates.platform, cover.*, screenshots.*, artworks.*, url, summary, aggregated_rating, "
      "involved_companies.company.name, involved_companies.developer, involved_companies.publisher ;";
  r.body += " search " + quote(name) + ";";
  r.body += " limit 10;";
  if (!platform_id.empty()) r.body += " where release_dates.platform = " + std::string(platform_id) + ";";
  r.post = true;
  return r;
}

bool valid_answer(std::string_view text) {
  json::Value v;
  return json::parse(text, v) && v.is_array();
}

bool parse_games(std::string_view text, const ss::Wish& wish, ss::Parsed& out, int max_px) {
  // size tokens by the resize setting (IGDB image sizes: cover_small 90x128, cover_big 264x374, screenshot_med 569x320, _big 889x500, _huge/720p 1280x720)
  const char* cover_tok = max_px <= 0 ? "t_original" : max_px <= 128 ? "t_cover_small" : max_px <= 374 ? "t_cover_big" : max_px <= 1280 ? "t_720p" : "t_1080p";
  const char* shot_tok = max_px <= 0 ? "t_screenshot_huge" : max_px <= 569 ? "t_screenshot_med" : max_px <= 889 ? "t_screenshot_big" : "t_screenshot_huge";
  json::Value v;
  if (!json::parse(text, v) || !v.is_array()) return false;
  for (const json::Value& g : v.arr) {
    if (!g.is_object()) continue;
    std::string name = g.text("name");
    if (name.empty()) continue;
    ss::Parsed p;
    p.name = name;
    p.id = g.text("id");
    const json::Value* sm = g.find("summary");
    if (sm && sm->type == json::Value::Type::String) p.desc = sm->str;
    // ES: every involved company that is flagged developer / publisher overwrites the previous one
    if (const json::Value* ic = g.find("involved_companies"); ic && ic->is_array()) {
      for (const json::Value& c : ic->arr) {
        const json::Value* co = c.find("company");
        if (!co || !co->is_object()) continue;
        const json::Value* cn = co->find("name");
        if (!cn || cn->type != json::Value::Type::String || cn->str.empty()) continue;
        if (c.boolean("developer")) p.developer = cn->str;
        if (c.boolean("publisher")) p.publisher = cn->str;
      }
    }
    if (const json::Value* cv = g.find("cover")) {
      std::string id = image_id(*cv);
      if (!id.empty()) p.picks.push_back({"thumb", std::string(kImg) + cover_tok + "/" + id + ".jpg", ".jpg"});
    }
    if (const json::Value* sc = g.find("screenshots"); sc && sc->is_array()) {
      for (const json::Value& s : sc->arr) {
        std::string id = image_id(s);
        if (id.empty()) continue;
        p.picks.push_back({"image", std::string(kImg) + shot_tok + "/" + id + ".jpg", ".jpg"});
        break;
      }
    }
    if (wish.fanart) {
      if (const json::Value* ar = g.find("artworks"); ar && ar->is_array()) {
        for (const json::Value& a : ar->arr) {
          std::string id = image_id(a);
          if (id.empty()) continue;
          p.picks.push_back({"fanart", std::string(kImg) + "t_1080p/" + id + ".jpg", ".jpg"});
          break;
        }
      }
    }
    out = std::move(p);
    return true;
  }
  return false;
}

}  // namespace zf::scraper::igdb
