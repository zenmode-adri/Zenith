#include "collections.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>

#include <pugixml.hpp>

#include "gamemeta.hpp"
#include "i18n.hpp"
#include "settings.hpp"
#include "util.hpp"

namespace zf::collections {

namespace {

bool g_reload = false;

std::string getenv_or(const char* k, const char* def) {
  const char* v = std::getenv(k);
  return v && *v ? v : def;
}

// Top level genres of ES's genres.xml in file order, without the ones ES drops by id (GENRE_LIGHTGUN 32, GENRE_WHEEL 33,
// GENRE_TRACKBALL 34, GENRE_SPINNER 35: that also removes "Board game" and "Compilation", as in ES).
const char* const kGenres[] = {
    N_("Action"),
    N_("Adult"),
    N_("Adventure"),
    N_("Beat'em Up"),
    N_("Casino"),
    N_("Casual Game"),
    N_("Hunting and Fishing"),
    N_("Fight"),
    N_("Race, Driving"),
    N_("Demo"),
    N_("Various"),
    N_("Pinball"),
    N_("Playing cards"),
    N_("Role playing games"),
    N_("Asiatic board game"),
    N_("Educational"),
    N_("Music and Dance"),
    N_("Platform"),
    N_("Puzzle-Game"),
    N_("Quiz"),
    N_("Reflection"),
    N_("Shoot'em Up"),
    N_("Simulation"),
    N_("Sports"),
    N_("Sports with Animals"),
    N_("Strategy"),
    N_("Shooter"),
};

void replace_all(std::string& s, std::string_view from, std::string_view to) {
  if (from.empty()) return;
  for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
}

// ES: lower case, then drop " ", "'", ",", "-" and "game".
std::string genre_short(std::string_view en) {
  std::string s;
  for (char c : en) s.push_back(c >= 'A' && c <= 'Z' ? char(c + 32) : c);
  replace_all(s, " ", "");
  replace_all(s, "'", "");
  replace_all(s, ",", "");
  replace_all(s, "-", "");
  replace_all(s, "game", "");
  return s;
}

bool valid_name_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '[' || c == ']' || c == '(' || c == ')' || c == ' ';
}

std::string xml_escape(std::string_view s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default: o.push_back(c);
    }
  }
  return o;
}

bool mkdir_p(const std::string& dir) {
  if (dir.empty() || is_dir(dir) || writes_blocked()) return true;
  size_t slash = dir.find_last_of('/');
  if (slash != std::string::npos && slash > 0 && !mkdir_p(dir.substr(0, slash))) return false;
  return ::mkdir(dir.c_str(), 0755) == 0 || is_dir(dir);
}

// lines of the cfg file; a line is a game when it is not blank, not a comment and does not start with '0' (ES)
void read_lines(const std::string& name, std::vector<std::string>& out) {
  std::string text;
  if (!read_file(cfg_path(name), text)) return;
  size_t i = 0;
  while (i < text.size()) {
    size_t e = text.find('\n', i);
    if (e == std::string::npos) e = text.size();
    std::string_view line = trim(std::string_view(text).substr(i, e - i));
    i = e + 1;
    if (line.empty() || line[0] == '0' || line[0] == '#') continue;
    out.emplace_back(line);
  }
}

bool write_lines(const std::string& name, const std::vector<std::string>& keys, std::string& err) {
  if (!mkdir_p(paths().dir)) {
    err = paths().dir + ": cannot create";
    return false;
  }
  std::string text;
  for (const std::string& k : keys) text += k + "\n";
  return write_atomic(cfg_path(name), text, &err);
}

}  // namespace

Paths& paths() {
  static Paths p = {getenv_or("ZF_COLLECTIONS_DIR", "/storage/.config/emulationstation/collections"), getenv_or("ZF_COLLECTIONS_ROOT", "/storage")};
  return p;
}

std::vector<std::string> split_list(std::string_view s, char sep) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i <= s.size()) {
    size_t e = s.find(sep, i);
    if (e == std::string_view::npos) e = s.size();
    if (e > i) out.emplace_back(s.substr(i, e - i));
    i = e + 1;
  }
  return out;
}

std::string join_sorted(std::vector<std::string> v) {
  std::sort(v.begin(), v.end());
  std::string o;
  for (const std::string& s : v) {
    if (!o.empty()) o += ',';
    o += s;
  }
  return o;
}

const std::vector<AutoDecl>& auto_decls() {
  static const std::vector<AutoDecl> d = [] {
    std::vector<AutoDecl> v = {
        {"all", N_("all games"), "auto-allgames", false},
        {"recent", N_("last played"), "auto-lastplayed", false},
        {"favorites", N_("favorites"), "auto-favorites", false},
        {"2players", N_("2 players"), "auto-at2players", false},
        {"4players", N_("4 players"), "auto-at4players", false},
        {"neverplayed", N_("never played"), "auto-neverplayed", false},
        {"retroachievements", N_("RetroAchievements"), "auto-retroachievements", false},
        {"arcade", N_("arcade"), "arcade", false},
        {"vertical", N_("vertical arcade"), "auto-verticalarcade", false},
        {"lightgun", N_("lightgun games"), "auto-lightgun", false},
        {"wheel", N_("wheel games"), "auto-wheel", false},
        {"trackball", N_("trackball games"), "auto-trackball", false},
        {"spinner", N_("spinner games"), "auto-spinner", false},
    };
    for (const char* g : kGenres) {
      std::string sh = genre_short(g);
      v.push_back({"_" + sh, g, "auto-" + sh, true});
    }
    return v;
  }();
  return d;
}

std::string label(const AutoDecl& d) {
  // genres: ES shows them in the UI language from genres.xml; here they are catalog keys (zfront.po has the Spanish ones, other
  // languages show the English name). The ES catalog keys of the other collections are the lower case names ("all games").
  if (d.genre) return tr(d.long_name.c_str());
  return utf8_cap_first(tr(d.long_name.c_str()));
}

std::vector<std::string> enabled_auto(const EsSettings& es) {
  const std::string* v = es.get("CollectionSystemsAuto");
  return split_list(v ? std::string_view(*v) : std::string_view("favorites, recent"), ',');
}

std::vector<std::string> enabled_custom(const EsSettings& es) {
  const std::string* v = es.get("CollectionSystemsCustom");
  return split_list(v ? std::string_view(*v) : std::string_view(), ',');
}

std::vector<std::string> hidden_systems(const EsSettings& es) {
  const std::string* v = es.get("HiddenSystems");
  return split_list(v ? std::string_view(*v) : std::string_view(), ';');
}

void set_member(std::vector<std::string>& vec, const std::string& name, bool on) {
  auto it = std::find(vec.begin(), vec.end(), name);
  if (on && it == vec.end()) vec.push_back(name);
  if (!on && it != vec.end()) vec.erase(it);
}

// ---- custom collections ---------------------------------------------------------------------------------------------------

std::string cfg_path(const std::string& name) { return path_join(paths().dir, "custom-" + name + ".cfg"); }
std::string xcc_path(const std::string& name) { return path_join(paths().dir, name + ".xcc"); }

std::vector<Custom> list_custom() {
  std::vector<Custom> out;
  DIR* d = ::opendir(paths().dir.c_str());
  if (!d) return out;
  while (dirent* e = ::readdir(d)) {
    std::string_view f = e->d_name;
    if (f == "." || f == "..") continue;
    if (e->d_type != DT_REG && e->d_type != DT_UNKNOWN && e->d_type != DT_LNK) continue;
    // ES: "custom-<name>.cfg" (not "custom-.cfg") or any "*.xcc"
    if (starts_with(f, "custom-") && ends_with(f, ".cfg") && f.size() > 11) {
      out.push_back({std::string(f.substr(7, f.size() - 11)), false});
    } else if (ends_with(f, ".xcc") && f.size() > 4) {
      out.push_back({std::string(f.substr(0, f.size() - 4)), true});
    }
  }
  ::closedir(d);
  std::sort(out.begin(), out.end(), [](const Custom& a, const Custom& b) { return a.name < b.name; });
  // a name present as both: the .xcc wins (ES creates the dynamic data when the index file exists)
  out.erase(std::unique(out.begin(), out.end(), [](const Custom& a, const Custom& b) { return a.name == b.name; }), out.end());
  return out;
}

std::vector<std::string> reserved_names() {
  std::vector<std::string> v = {"collections", "custom-collections"};
  for (const AutoDecl& d : auto_decls()) {
    v.push_back(d.name);
    v.push_back(d.theme_folder);
  }
  for (const Custom& c : list_custom()) v.push_back(c.name);
  return v;
}

std::string valid_new_name(std::string_view wanted, const std::vector<std::string>& taken) {
  std::string base;
  for (char c : wanted)
    if (valid_name_char(c)) base.push_back(c);
  if (base.empty()) base = "New Collection";
  auto used = [&](const std::string& n) { return std::find(taken.begin(), taken.end(), n) != taken.end(); };
  if (!used(base)) return base;
  for (int i = 1; i < 10000; ++i) {
    std::string n = base + " (" + std::to_string(i) + ")";
    if (!used(n)) return n;
  }
  return base;
}

bool create_editable(const std::string& name, std::string& err) { return write_lines(name, {}, err); }

std::string filter_xml(const std::string& name, const Filter& f) {
  std::string o = "<?xml version=\"1.0\"?>\n<filter name=\"" + xml_escape(name) + "\">\n";
  for (const std::string& s : f.systems) o += "  <system>" + xml_escape(s) + "</system>\n";
  if (!f.text.empty()) o += "  <text>" + xml_escape(f.text) + "</text>\n";
  if (f.only_favorites) o += "  <favorites>TRUE</favorites>\n";
  o += "</filter>\n";
  return o;
}

bool create_dynamic(const std::string& name, const Filter& f, std::string& err) {
  if (!mkdir_p(paths().dir)) {
    err = paths().dir + ": cannot create";
    return false;
  }
  return write_atomic(xcc_path(name), filter_xml(name, f), &err);
}

bool read_filter(const std::string& name, Filter& f) {
  std::string text;
  if (!read_file(xcc_path(name), text)) return false;
  pugi::xml_document doc;
  if (!doc.load_buffer(text.data(), text.size())) return false;
  pugi::xml_node root = doc.child("filter");
  if (!root) return false;
  f = Filter();
  for (pugi::xml_node n = root.first_child(); n; n = n.next_sibling()) {
    std::string tag = n.name(), val = n.text().as_string();
    if (tag == "system") f.systems.push_back(val);
    else if (tag == "text") f.text = val;
    else if (tag == "favorites") f.only_favorites = val == "TRUE";
  }
  return true;
}

namespace {
struct StrWriter : pugi::xml_writer {
  std::string out;
  void write(const void* data, size_t size) override { out.append(static_cast<const char*>(data), size); }
};
}  // namespace

bool write_filter(const std::string& name, const Filter& f, std::string& err) {
  std::string text;
  pugi::xml_document doc;
  if (!read_file(xcc_path(name), text) || !doc.load_buffer(text.data(), text.size()) || !doc.child("filter")) return create_dynamic(name, f, err);
  pugi::xml_node root = doc.child("filter");
  for (pugi::xml_node n = root.first_child(); n;) {
    pugi::xml_node next = n.next_sibling();
    std::string_view tag = n.name();
    if (tag == "system" || tag == "text" || tag == "favorites") root.remove_child(n);
    n = next;
  }
  for (const std::string& s : f.systems) root.append_child("system").text().set(s.c_str());
  if (!f.text.empty()) root.append_child("text").text().set(f.text.c_str());
  if (f.only_favorites) root.append_child("favorites").text().set("TRUE");
  StrWriter w;
  doc.save(w, "  ", pugi::format_default | pugi::format_no_declaration);
  return write_atomic(xcc_path(name), w.out, &err);
}

bool remove_custom(const std::string& name, EsSettings& es, std::string& err) {
  // ES looks for the .xcc first, then the .cfg
  std::string path = xcc_path(name);
  if (!file_exists(path)) path = cfg_path(name);
  if (!file_exists(path)) {
    err = name + ": no such collection";
    return false;
  }
  std::vector<std::string> on = enabled_custom(es);
  set_member(on, name, false);
  es.set_string("CollectionSystemsCustom", join_sorted(on));
  if (!writes_blocked() && ::unlink(path.c_str()) != 0) {
    err = path + ": cannot delete";
    return false;
  }
  return true;
}

std::string to_key(const std::string& abs_path) {
  const std::string& root = paths().root;
  if (!root.empty() && abs_path.size() > root.size() && abs_path.compare(0, root.size(), root) == 0 && abs_path[root.size()] == '/') return "." + abs_path.substr(root.size());
  return abs_path;
}

std::string from_key(const std::string& key) {
  if (key.size() >= 2 && key[0] == '.' && key[1] == '/') return paths().root + key.substr(1);
  if (key.size() >= 2 && key[0] == '~' && key[1] == '/') {
    const char* home = std::getenv("HOME");
    return std::string(home && *home ? home : paths().root.c_str()) + key.substr(1);
  }
  return key;
}

std::vector<std::string> members(const std::string& name) {
  std::vector<std::string> keys, out;
  read_lines(name, keys);
  out.reserve(keys.size());
  for (const std::string& k : keys) out.push_back(from_key(k));
  return out;
}

bool contains(const std::string& name, const std::string& abs_path) {
  std::vector<std::string> keys;
  read_lines(name, keys);
  for (const std::string& k : keys)
    if (from_key(k) == abs_path) return true;
  return false;
}

bool add_game(const std::string& name, const std::string& abs_path, std::string& err, bool* changed) {
  if (changed) *changed = false;
  std::vector<std::string> keys;
  read_lines(name, keys);
  for (const std::string& k : keys)
    if (from_key(k) == abs_path) return true;  // already there
  keys.push_back(to_key(abs_path));
  if (!write_lines(name, keys, err)) return false;
  if (changed) *changed = true;
  return true;
}

bool remove_game(const std::string& name, const std::string& abs_path, std::string& err, bool* changed) {
  if (changed) *changed = false;
  std::vector<std::string> keys;
  read_lines(name, keys);
  size_t before = keys.size();
  keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const std::string& k) { return from_key(k) == abs_path; }), keys.end());
  if (keys.size() == before) return true;
  if (!write_lines(name, keys, err)) return false;
  if (changed) *changed = true;
  return true;
}

// ---- favourites -------------------------------------------------------------------------------------------------------------

std::string fmt(const char* format, const std::string& a, const std::string& b) {
  std::string out, f = format;
  const std::string* args[2] = {&a, &b};
  int n = 0;
  for (size_t i = 0; i < f.size(); ++i) {
    if (f[i] == '%' && i + 1 < f.size() && f[i + 1] == 's' && n < 2) {
      out += *args[n++];
      ++i;
    } else {
      out.push_back(f[i]);
    }
  }
  return out;
}

bool toggle_favorite(Ctx& cx, size_t sys, size_t game) {
  if (sys >= cx.systems.size() || game >= cx.systems[sys].games.size()) return false;
  const Game& g = cx.systems[sys].games[game];
  bool now = !g.meta.favorite;
  std::string name = g.name;  // copy: game_set_favorite keeps the vector, but the toast text must not depend on it
  game_set_favorite(cx, sys, game, now);
  cx.sv->toasts.push(fmt(now ? tr("Added '%s' to '%s'") : tr("Removed '%s' from '%s'"), name, tr("FAVORITES")), 2500);
  return true;
}

bool reload_needed() { return g_reload; }
void set_reload_needed(bool v) { g_reload = v; }

}  // namespace zf::collections
