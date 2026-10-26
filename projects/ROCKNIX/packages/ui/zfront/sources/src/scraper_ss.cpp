#include "scraper_ss.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "pugixml.hpp"
#include "util.hpp"

#ifndef ZF_SS_DEV_LOGIN
#define ZF_SS_DEV_LOGIN ""
#endif
#ifndef ZF_SS_SOFTNAME
#define ZF_SS_SOFTNAME "zfront"
#endif

namespace zf::scraper::ss {

std::string dev_login() {
  if (const char* e = std::getenv("ZF_SS_DEVLOGIN"); e && *e) return e;  // tests / debugging over SSH
  return ZF_SS_DEV_LOGIN;
}

std::string soft_name() {
  if (const char* e = std::getenv("ZF_SS_SOFTNAME"); e && *e) return e;
  return ZF_SS_SOFTNAME;
}

// ---- platforms ---------------------------------------------------------------------------------------------------------
namespace {

struct PlatId {
  const char* name;
  int id;
};
// ES platform name -> ScreenScraper system id (from ES's PlatformId.cpp + ScreenScraper.cpp)
const PlatId kPlat[] = {
    {"3do", 29}, {"3ds", 17}, {"actionmax", 81}, {"adam", 89}, {"advision", 78}, {"amiga", 64}, {"amigacd32", 130},
    {"amigacdtv", 129}, {"amstradcpc", 65}, {"apple2", 86}, {"apple2gs", 217}, {"arcade", 75}, {"arcadia", 94},
    {"archimedes", 84}, {"arduboy", 263}, {"astrocade", 44}, {"astrocde", 44}, {"atari2600", 26}, {"atari5200", 40},
    {"atari7800", 41}, {"atari800", 43}, {"atarijaguar", 27}, {"atarijaguarcd", 171}, {"atarilynx", 28},
    {"atarist", 42}, {"atom", 36}, {"atomiswave", 53}, {"bbc", 37}, {"bbcmicro", 37}, {"bk", 93}, {"c16", 99},
    {"c20", 73}, {"c64", 66}, {"camplynx", 88}, {"cassettevision", 300}, {"cavestory", 135}, {"cdi", 133},
    {"channelf", 80}, {"coco", 144}, {"colecovision", 48}, {"cpet", 240}, {"cplus4", 99}, {"creativision", 241},
    {"crvision", 241}, {"daphne", 49}, {"dragon32", 91}, {"dreamcast", 23}, {"easyrpg", 231}, {"electron", 85},
    {"fds", 106}, {"fm7", 97}, {"fmtowns", 253}, {"fpinball", 199}, {"gamate", 266}, {"gameandwatch", 52},
    {"gamecom", 121}, {"gamegear", 21}, {"gamepock", 95}, {"gb", 9}, {"gb2players", 9}, {"gba", 12}, {"gbc", 10},
    {"gbc2players", 10}, {"gc", 13}, {"genesis", 1}, {"gmaster", 103}, {"gp32", 101}, {"gx4000", 87},
    {"intellivision", 115}, {"j2me", 302}, {"jaguar", 27}, {"lcdgames", 75}, {"loopy", 98}, {"lowresnx", 244},
    {"lutro", 206}, {"macintosh", 146}, {"mastersystem", 2}, {"megacd", 20}, {"megadrive", 1}, {"megaduck", 90},
    {"model3", 55}, {"moonlight", 138}, {"msx", 113}, {"msx2", 116}, {"msx2+", 117}, {"msx2turbor", 118},
    {"multivision", 109}, {"n3ds", 17}, {"n64", 14}, {"n64dd", 122}, {"naomi", 56}, {"nds", 15}, {"neogeo", 142},
    {"neogeocd", 70}, {"nes", 3}, {"ngage", 30}, {"ngp", 25}, {"ngpc", 82}, {"o2em", 104}, {"odyssey2", 104},
    {"openbor", 214}, {"oric", 131}, {"oricatmos", 131}, {"palm", 219}, {"pc", 135}, {"pc88", 221}, {"pc98", 208},
    {"pcarcade", 269}, {"pcengine", 31}, {"pcenginecd", 114}, {"pcfx", 72}, {"pet", 240}, {"pico", 250},
    {"pico-8", 234}, {"pico8", 234}, {"pokemini", 211}, {"prboom", 135}, {"ps2", 58}, {"ps3", 59}, {"ps4", 60},
    {"psp", 61}, {"psvita", 62}, {"psx", 57}, {"pv1000", 74}, {"samcoupe", 213}, {"satellaview", 107}, {"saturn", 22},
    {"scummvm", 123}, {"scv", 67}, {"sega32x", 19}, {"segacd", 20}, {"sg-1000", 109}, {"sg1000", 109}, {"sgb", 127},
    {"snes", 4}, {"snes-msu1", 210}, {"solarus", 223}, {"spectravideo", 218}, {"sufami", 108}, {"supergrafx", 105},
    {"supervision", 207}, {"supracan", 100}, {"switch", 225}, {"teknoparrot", 269}, {"thomson", 141}, {"ti99", 205},
    {"ti99_4a", 205}, {"tic80", 222}, {"trs-80", 144}, {"tyrquake", 135}, {"uzebox", 216}, {"vc4000", 281},
    {"vectrex", 102}, {"vic20", 73}, {"videopac", 104}, {"vircon32", 272}, {"virtualboy", 11}, {"vpinball", 198},
    {"vsmile", 120}, {"wasm4", 262}, {"watara", 207}, {"wii", 16}, {"wiiu", 18}, {"windows", 138}, {"wonderswan", 45},
    {"wonderswancolor", 46}, {"x1", 220}, {"x68000", 79}, {"xbox", 32}, {"xbox360", 33}, {"xegs", 43}, {"zx81", 77},
    {"zxspectrum", 76},
};

std::vector<std::string_view> words(std::string_view s) {
  std::vector<std::string_view> w;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    size_t j = i;
    while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
    if (j > i) w.push_back(s.substr(i, j - i));
    i = j;
  }
  return w;
}

std::string lower(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = char(std::tolower(static_cast<unsigned char>(c)));
  return o;
}

}  // namespace

std::vector<int> system_ids(std::string_view platforms) {
  std::vector<int> ids;
  for (std::string_view w : words(platforms))
    for (const PlatId& p : kPlat)
      if (w == p.name) ids.push_back(p.id);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

bool platform_supported(std::string_view platforms) { return !system_ids(platforms).empty(); }

// ---- URLs ------------------------------------------------------------------------------------------------------------
std::string url_encode(std::string_view s) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o.push_back(char(c));
    else {
      o.push_back('%');
      o.push_back(hex[c >> 4]);
      o.push_back(hex[c & 15]);
    }
  }
  return o;
}

namespace {
std::string base_url(const char* script, const Login& l) {
  return std::string("https://api.screenscraper.fr/api2/") + script + "?" + l.dev + "&softname=" + url_encode(l.soft) + "&output=xml";
}
void add_login(std::string& u, const Login& l) {
  if (!l.user.empty() && !l.pass.empty()) u += "&ssid=" + url_encode(l.user) + "&sspassword=" + url_encode(l.pass);
}
}  // namespace

std::string game_url(const Login& l, const GameQuery& q) {
  std::string u = base_url("jeuInfos.php", l) + "&romnom=" + url_encode(q.rom_name) + "&romtype=rom";
  if (!q.md5.empty()) u += "&md5=" + url_encode(q.md5);
  if (!q.crc.empty()) u += "&crc=" + url_encode(q.crc);
  if (q.size) u += "&romtaille=" + std::to_string(q.size);
  if (q.system_id) u += "&systemeid=" + std::to_string(q.system_id);
  add_login(u, l);
  return u;
}

std::string user_url(const Login& l) {
  std::string u = base_url("ssuserInfos.php", l);
  add_login(u, l);
  return u;
}

std::string scrub(std::string_view s) {
  static const char* keys[] = {"devid", "devpassword", "ssid", "sspassword"};
  std::string o(s);
  for (const char* k : keys) {
    size_t klen = std::strlen(k);
    size_t pos = 0;
    while ((pos = o.find(k, pos)) != std::string::npos) {
      bool boundary = pos == 0 || !std::isalnum(static_cast<unsigned char>(o[pos - 1]));
      if (!boundary || pos + klen >= o.size() || o[pos + klen] != '=') {
        pos += klen;
        continue;
      }
      size_t b = pos + klen + 1, e = b;
      while (e < o.size() && o[e] != '&' && o[e] != '"' && o[e] != '\'' && o[e] != ')' && !std::isspace(static_cast<unsigned char>(o[e]))) ++e;
      o.replace(b, e - b, "***");
      pos = b + 3;
    }
  }
  return o;
}

// ---- ROM region / language (ES LangParser) -------------------------------------------------------------------------------
namespace {

struct LangData {
  std::vector<const char*> value;
  const char* lang;
  const char* region;
};

const std::vector<LangData>& lang_data() {
  static const std::vector<LangData> d = {
      {{"usa", "us", "u"}, "en", "us"},
      {{"europe", "eu", "e", "ue", "euro"}, "", "eu"},
      {{"w", "wor", "world"}, "en", "wr"},
      {{"uk", "gb"}, "en", "eu"},
      {{"es", "spain", "s"}, "es", "eu"},
      {{"fr", "france", "fre", "french", "f"}, "fr", "eu"},
      {{"de", "germany", "d"}, "de", "eu"},
      {{"it", "italy", "i"}, "it", "eu"},
      {{"nl", "netherlands"}, "nl", "eu"},
      {{"gr", "greece"}, "gr", "eu"},
      {{"no"}, "no", "eu"},
      {{"sw", "sweden", "se"}, "sw", "eu"},
      {{"pt", "portugal"}, "pt", "eu"},
      {{"pl", "poland"}, "pl", "eu"},
      {{"en"}, "en", ""},
      {{"jp", "japan", "ja", "j"}, "jp", "jp"},
      {{"br", "brazil"}, "br", "br"},
      {{"ru", "r"}, "ru", "ru"},
      {{"kr", "korea", "k"}, "kr", "kr"},
      {{"cn", "china", "hong", "kong", "ch", "hk", "as", "tw"}, "cn", "cn"},
      {{"canada", "ca", "c", "fc"}, "fr", "wr"},
      {{"in"}, "in", "in"},
  };
  return d;
}

bool has(const LangData& d, const std::string& s) {
  for (const char* v : d.value)
    if (s == v) return true;
  return false;
}

std::vector<std::string> extract_between(const std::string& s, char open, char close) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t b = s.find(open, pos);
    if (b == std::string::npos) break;
    size_t e = s.find(close, b + 1);
    if (e == std::string::npos) break;
    if (e > b + 1) out.push_back(s.substr(b + 1, e - b - 1));
    pos = e + 1;
  }
  return out;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
  for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
  return s;
}

struct LangState {
  RomLang info;
  bool hard_region = false;
  std::string sys_lang = "en", sys_region;
};

void extract_lang(const std::string& val, LangState& st) {
  static const std::set<std::string> hard = {"usa", "us", "europe", "eu", "brazil", "br", "japan", "jp", "kr", "korea"};
  std::vector<std::string> toks;
  {
    std::string cur;
    for (char c : val) {
      if (c == '_' || c == ',' || c == ' ') {
        if (!cur.empty()) toks.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    if (!cur.empty()) toks.push_back(cur);
  }
  for (std::string s : toks) {
    bool clear_lang = s.find("t-") != std::string::npos;
    s = replace_all(replace_all(s, "t+", ""), "t-", "");
    for (const LangData& d : lang_data()) {
      if (!has(d, s)) continue;
      if (d.lang[0]) {
        if (clear_lang) st.info.languages.clear();
        if (std::find(st.info.languages.begin(), st.info.languages.end(), d.lang) == st.info.languages.end()) st.info.languages.push_back(d.lang);
      }
      if (d.region[0]) {
        bool new_hard = hard.count(s) != 0;
        if (!st.hard_region) {
          st.info.region = d.region;
          st.hard_region = new_hard;
        } else if (new_hard && st.info.region != st.sys_region) {
          if (std::string(d.region) == st.sys_region || std::string(d.lang) == st.sys_lang) st.info.region = d.region;
          else if (!d.lang[0] && std::string(d.region) == "eu")
            for (const LangData& ld : lang_data())
              if (std::string(ld.region) == "eu" && std::string(ld.lang) == st.sys_lang) st.info.region = ld.region;
        }
      }
    }
  }
}

bool in_list(const char* const* l, size_t n, const std::string& s) {
  for (size_t i = 0; i < n; ++i)
    if (s == l[i]) return true;
  return false;
}

}  // namespace

RomLang rom_lang(std::string_view file, std::string_view system_name, std::string_view platforms, std::string_view locale) {
  LangState st;
  std::string loc(locale);
  size_t div = loc.find('_');
  if (div != std::string::npos) {
    st.sys_lang = lower(loc.substr(0, div));
    st.sys_region = lower(loc.substr(div + 1));
    for (const LangData& d : lang_data())
      if (has(d, st.sys_region)) st.sys_region = d.region;
  }
  std::string name = lower(path_filename(file));
  bool arcade = false;
  for (std::string_view w : words(platforms))
    if (w == "arcade" || w == "neogeo") arcade = true;
  for (const std::string& s : extract_between(name, '(', ')')) extract_lang(s, st);
  for (const std::string& s : extract_between(name, '[', ']')) extract_lang(s, st);
  for (const std::string& s : extract_between(name, '_', '_')) extract_lang(s, st);
  static const char* const kJapan[] = {"pc88", "pc98", "pcenginecd", "pcfx", "satellaview", "sg1000", "sufami", "wswan", "wswanc", "x68000"};
  std::string sys(system_name);
  bool jp_sys = in_list(kJapan, sizeof kJapan / sizeof *kJapan, sys);
  bool j_zip = name.find("j.zip") != std::string::npos;
  if (st.info.region.empty()) {
    if (jp_sys) st.info.region = "jp";
    else if (arcade) st.info.region = j_zip ? "jp" : "us";
    else if (sys == "thomson") st.info.region = "eu";
  }
  if (st.info.languages.empty()) {
    if (jp_sys) st.info.languages.push_back("jp");
    else if (sys == "thomson") st.info.languages.push_back("fr");
    else if (arcade && j_zip) st.info.languages.push_back("jp");
    else st.info.languages.push_back("en");
  }
  return st.info;
}

LangPick pick_lang(const RomLang& rl, std::string_view locale) {
  static const char* const kEu[] = {"fr", "es", "ca", "el", "hu", "it", "sv", "uk", "gr", "no", "sw", "nl", "de", "pl"};
  LangPick p;
  std::string region = "us";
  if (!rl.region.empty()) region = rl.region;
  std::string language(locale);
  if (language.empty()) {
    language = "en";
  } else {
    size_t div = language.find('_');
    if (div != std::string::npos) {
      if (rl.region.empty()) region = lower(language.substr(div + 1));
      language = lower(language.substr(0, div));
    }
    if (rl.region.empty() && div != std::string::npos) {
      if (language == "es" && region == "mx") region = "mx";
      else if (language == "pt" && region == "br") region = "br";
      else if (in_list(kEu, sizeof kEu / sizeof *kEu, language)) region = "eu";
    }
  }
  if (std::find(rl.languages.begin(), rl.languages.end(), language) != rl.languages.end()) p.romlang = language;
  else if (rl.languages.size() == 1) p.romlang = rl.languages[0];
  else p.romlang = region;
  p.language = language;
  p.region = region;
  return p;
}

// ---- classification ----------------------------------------------------------------------------------------------------
Reply classify(int http, std::string_view body) {
  if (http >= 200 && http <= 299) {
    std::string b = lower(body.substr(0, 400));
    if (b.find("maximum threads per minute reached") != std::string::npos) return Reply::TooFast;
    std::string_view t = trim(body);
    if (starts_with(t, "<html") || starts_with(t, "<!DOCTYPE") || starts_with(t, "<!doctype")) return Reply::Transient;
    return Reply::Ok;
  }
  switch (http) {
    case 400:
    case 404:
      return Reply::NotFound;
    case 401:
      return Reply::ApiClosed;
    case 403:
      return Reply::BadLogin;
    case 423:
      return Reply::Maintenance;
    case 426:
      return Reply::Blacklisted;
    case 429:
      return Reply::TooFast;
    case 430:
      return Reply::Quota;
    case 431:
      return Reply::TooManyFails;
    default:
      return Reply::Transient;
  }
}

bool is_fatal(Reply r) {
  return r == Reply::BadLogin || r == Reply::ApiClosed || r == Reply::Maintenance || r == Reply::Blacklisted || r == Reply::Quota || r == Reply::TooManyFails;
}

// ---- answer parsing ----------------------------------------------------------------------------------------------------
namespace {

using pugi::xml_node;

// first child `name` whose attribute `attr` equals one of `values`, in the order of `values`
xml_node child_by_attr(const xml_node& parent, const char* name, const char* attr, const std::vector<std::string>& values) {
  for (const std::string& v : values)
    for (xml_node n : parent.children(name))
      if (v == n.attribute(attr).value()) return n;
  return xml_node();
}

std::string decode_entities(std::string s) {
  static const std::pair<const char*, const char*> e[] = {{"&quot;", "\""}, {"&apos;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
  for (auto& kv : e) s = replace_all(std::move(s), kv.first, kv.second);
  return s;
}

std::string node_text(const xml_node& n) { return std::string(trim(n.text().get())); }

std::string media_text(const xml_node& n) {
  std::string t = node_text(n);
  if (t.empty()) t = n.attribute("url").value();
  return t;
}

xml_node find_media(const xml_node& medias, const std::vector<std::string>& names, const std::string& language, const std::string& region) {
  const std::vector<std::string> regions = {language, region, "wor", "us", "eu", "jp", "ss", "cus", ""};
  for (const std::string& name : names) {
    std::vector<xml_node> cand;
    for (xml_node m : medias.children("media"))
      if (name == m.attribute("type").value()) cand.push_back(m);
    if (cand.empty()) continue;
    for (const std::string& r : regions)
      for (const xml_node& m : cand)
        if (r == m.attribute("region").value()) return m;
  }
  return xml_node();
}

}  // namespace

std::vector<std::string> rip_list(const std::string& src) {
  if (src == "ss") return {"ss", "sstitle"};
  if (src == "sstitle") return {"sstitle", "ss"};
  if (src == "mixrbv1" || src == "mixrbv") return {"mixrbv1", "mixrbv2"};
  if (src == "mixrbv2") return {"mixrbv2", "mixrbv1"};
  if (src == "box-2D") return {"box-2D", "box-3D"};
  if (src == "box-3D") return {"box-3D", "box-2D"};
  if (src == "wheel") return {"wheel", "wheel-hd", "wheel-steel", "wheel-carbon", "screenmarqueesmall", "screenmarquee"};
  if (src == "wheel-hd") return {"wheel-hd", "wheel", "wheel-steel", "wheel-carbon", "screenmarqueesmall", "screenmarquee"};
  if (src == "marquee") return {"screenmarqueesmall", "screenmarquee", "wheel", "wheel-hd", "wheel-steel", "wheel-carbon"};
  if (src == "video") return {"video-normalized", "video"};
  if (src.empty()) return {};
  return {src};
}

std::string media_url(std::string_view url, int max_px, bool is_image) {
  std::string u = replace_all(replace_all(std::string(url), " ", "%20"), "#screenscraperserveur#", "https://www.screenscraper.fr/");
  if (is_image && max_px > 0 && u.find("media=map") == std::string::npos) {
    // drop an existing size, then ask for ours
    for (const char* k : {"maxwidth", "maxheight"}) {
      for (size_t p = u.find(std::string("&") + k + "="); p != std::string::npos; p = u.find(std::string("&") + k + "=")) {
        size_t e = u.find('&', p + 1);
        u.erase(p, e == std::string::npos ? std::string::npos : e - p);
      }
    }
    u += (u.find('?') == std::string::npos ? "?" : "&");
    u += "maxwidth=" + std::to_string(max_px) + "&maxheight=" + std::to_string(max_px);
  }
  return u;
}

bool parse_game(std::string_view xml, const LangPick& lp, const Wish& w, Parsed& out) {
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size())) return false;
  xml_node data = doc.child("Data");
  if (data.child("jeux")) data = data.child("jeux");
  xml_node game = data.child("jeu");
  if (!game) return false;
  out = Parsed();
  const std::string& language = lp.language;
  std::string region = lp.region, romlang = lp.romlang;

  out.id = game.attribute("id").value();

  // name: region fallbacks
  if (region == "jp" && language != "jp") out.name = node_text(child_by_attr(game.child("noms"), "nom", "region", {region, "wor", "us", "ss", "eu", "jp"}));
  else out.name = node_text(child_by_attr(game.child("noms"), "nom", "region", {romlang, region, "wor", "us", "ss", "eu", "jp"}));

  // description: language, English, world
  std::string desc = node_text(child_by_attr(game.child("synopsis"), "synopsis", "langue", {language, "en", "wor"}));
  if (!desc.empty()) out.desc = decode_entities(desc);

  // genre
  if (game.child("genres")) {
    std::string genre, sub;
    for (xml_node n : game.child("genres").children("genre")) {
      std::string pr = n.attribute("principale").value(), lg = n.attribute("langue").value();
      if (pr == "1" && lg == language) genre = node_text(n);
      if (pr == "0" && lg == language) sub = node_text(n);
    }
    if (language != "en") {
      for (xml_node n : game.child("genres").children("genre")) {
        std::string pr = n.attribute("principale").value(), lg = n.attribute("langue").value();
        if (genre.empty() && pr == "1" && lg == "en") genre = node_text(n);
        if (sub.empty() && pr == "0" && lg == "en") sub = node_text(n);
      }
    }
    size_t sep = genre.find('/');
    if (sep != std::string::npos) genre = std::string(trim(std::string_view(genre).substr(0, sep)));
    sep = sub.find('/');
    if (genre.empty() || sep != std::string::npos) genre = sub;
    else if (!genre.empty() && !sub.empty()) genre = genre + " / " + sub;
    out.genre = genre;
  }

  // family: main one in our language, then in English, then any
  if (game.child("familles")) {
    std::string fam;
    for (const char* lg : {language.c_str(), "en"}) {
      for (xml_node n : game.child("familles").children("famille"))
        if (std::strcmp(n.attribute("principale").value(), "1") == 0 && std::strcmp(n.attribute("langue").value(), lg) == 0) {
          fam = node_text(n);
          break;
        }
      if (!fam.empty()) break;
    }
    if (fam.empty())
      for (xml_node n : game.child("familles").children("famille")) {
        fam = node_text(n);
        break;
      }
    out.family = fam;
  }

  // release date: YYYY-MM-DD or YYYY
  std::string date = node_text(child_by_attr(game.child("dates"), "date", "region", {region, "wor", "us", "ss", "eu", "jp"}));
  auto digits = [](const std::string& s, size_t b, size_t n) {
    if (s.size() < b + n) return false;
    for (size_t i = b; i < b + n; ++i)
      if (s[i] < '0' || s[i] > '9') return false;
    return true;
  };
  if (digits(date, 0, 4)) {
    if (date.size() >= 10 && digits(date, 5, 2) && digits(date, 8, 2)) out.date = date.substr(0, 4) + date.substr(5, 2) + date.substr(8, 2) + "T000000";
    else if (date.size() >= 7 && digits(date, 5, 2)) out.date = date.substr(0, 4) + date.substr(5, 2) + "01T000000";
    else out.date = date.substr(0, 4) + "0101T000000";
  }

  out.developer = replace_all(node_text(game.child("developpeur")), "&nbsp;", " ");
  out.publisher = replace_all(node_text(game.child("editeur")), "&nbsp;", " ");
  out.players = node_text(game.child("joueurs"));

  if (w.ratings && game.child("note")) {
    float v = float(game.child("note").text().as_int()) / 20.0f;
    char b[32];
    std::snprintf(b, sizeof b, "%g", double(v));
    out.rating = b;
  }

  // media
  xml_node medias = game.child("medias");
  if (medias) {
    std::string ml = romlang, rg = region;
    if (!w.region.empty()) {  // ScraperRegion applies only to medias
      rg = romlang;
      ml = w.region;
    }
    auto add = [&](const char* kind, const std::vector<std::string>& names) {
      if (names.empty()) return;
      xml_node m = find_media(medias, names, ml, rg);
      if (!m) return;
      Pick p;
      p.kind = kind;
      p.url = media_text(m);
      if (p.url.empty()) return;
      if (const char* f = m.attribute("format").value(); *f) p.ext = std::string(".") + f;
      out.picks.push_back(std::move(p));
    };
    add("image", rip_list(w.image_src));
    if (!w.thumb_src.empty() && w.thumb_src != w.image_src) add("thumb", rip_list(w.thumb_src));
    if (!w.logo_src.empty()) add("marquee", rip_list(w.logo_src));
    if (w.video) add("video", rip_list("video"));
    if (w.fanart) add("fanart", {"fanart"});
    if (w.boxback) add("boxback", {"box-2D-back"});
    if (w.manual) add("manual", {"manuel"});
    if (w.map) add("map", {"maps"});
    if (w.titleshot) add("titleshot", rip_list("sstitle"));
  }
  return true;
}

bool parse_user(std::string_view xml, UserInfo& out) {
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size())) return false;
  xml_node u = doc.child("Data").child("ssuser");
  if (!u) return false;
  out = UserInfo();
  out.id = node_text(u.child("id"));
  out.maxthreads = u.child("maxthreads").text().as_int();
  out.requests_today = u.child("requeststoday").text().as_int();
  out.requests_ko_today = u.child("requestskotoday").text().as_int();
  out.max_per_min = u.child("maxrequestspermin").text().as_int();
  out.max_per_day = u.child("maxrequestsperday").text().as_int();
  out.max_ko_per_day = u.child("maxrequestskoperday").text().as_int();
  return true;
}

}  // namespace zf::scraper::ss
