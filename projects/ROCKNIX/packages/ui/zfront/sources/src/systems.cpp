#include "systems.hpp"

#include <algorithm>
#include <pugixml.hpp>

#include "scan.hpp"
#include "util.hpp"

namespace zf {

namespace {

std::string text_of(pugi::xml_node n, const char* tag) {
  pugi::xml_node c = n.child(tag);
  return c ? std::string(trim(c.child_value())) : std::string();
}

// Default emulator/core as ES picks them (`<core default="true">`, else the first one).
std::pair<std::string, std::string> default_emu(pugi::xml_node sys) {
  pugi::xml_node emus = sys.child("emulators");
  if (!emus) return {};
  bool have_first = false;
  std::pair<std::string, std::string> first;
  for (pugi::xml_node e : emus.children("emulator")) {
    std::string en = e.attribute("name").value();
    pugi::xml_node cores = e.child("cores");
    if (!cores) continue;
    int i = 0;
    for (pugi::xml_node c : cores.children("core")) {
      std::string cn(trim(c.child_value()));
      if (!have_first && i == 0) {
        first = {en, cn};
        have_first = true;
      }
      if (std::string_view(c.attribute("default").value()) == "true") return {en, cn};
      ++i;
    }
  }
  return first;
}

std::vector<std::pair<std::string, std::vector<std::string>>> all_emus(pugi::xml_node sys) {
  std::vector<std::pair<std::string, std::vector<std::string>>> out;
  pugi::xml_node emus = sys.child("emulators");
  for (pugi::xml_node e : emus.children("emulator")) {
    std::vector<std::string> cores;
    for (pugi::xml_node c : e.child("cores").children("core")) cores.emplace_back(trim(c.child_value()));
    out.emplace_back(e.attribute("name").value(), std::move(cores));
  }
  return out;
}

// Gamelist file ES would read: `<romdir>/gamelist.xml`, else the per-system one under the ES config folder.
std::string gamelist_for(const std::string& name, const std::string& dir) {
  std::string p = path_join(dir, "gamelist.xml");
  if (file_exists(p)) return p;
  std::string alt = "/storage/.config/emulationstation/gamelists/" + name + "/gamelist.xml";
  return file_exists(alt) ? alt : p;
}

void collect_systems(pugi::xml_node n, std::vector<pugi::xml_node>& out) {
  for (pugi::xml_node c : n.children()) {
    if (c.type() != pugi::node_element) continue;
    if (std::string_view(c.name()) == "system") out.push_back(c);
    collect_systems(c, out);
  }
}

}  // namespace

std::string sort_key(const std::string& sort) {
  std::string u = utf8_upper(sort);
  for (const char* art : {"THE ", "AN ", "A "}) {
    if (starts_with(u, art)) return u.substr(std::string_view(art).size());
  }
  return u;
}

namespace {

// Sort key into `buf`, same result as `sort_key` (upper case, leading "THE "/"AN "/"A " dropped) without a
// temporary string in the ASCII case.
void append_key(std::string& buf, std::string_view s) {
  bool ascii = true;
  for (char c : s)
    if (static_cast<unsigned char>(c) >= 0x80) {
      ascii = false;
      break;
    }
  if (!ascii) {
    buf += sort_key(std::string(s));
    return;
  }
  auto up = [](char c) { return c >= 'a' && c <= 'z' ? char(c - 32) : c; };
  auto art = [&](std::string_view a) {
    if (s.size() < a.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (up(s[i]) != a[i]) return false;
    return true;
  };
  size_t skip = art("THE ") ? 4 : art("AN ") ? 3 : art("A ") ? 2 : 0;
  for (size_t i = skip; i < s.size(); ++i) buf.push_back(up(s[i]));
}

inline uint64_t hash_bytes(std::string_view s) {  // FNV-1a
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
  return h;
}

}  // namespace

std::vector<Game> load_games(const std::string& dir, const std::vector<std::string>& exts, const std::string& gamelist) {
  GameList files = GameList::scan(dir, Exts(exts));  // names in one buffer, one string per folder
  const size_t n = files.size();
  std::vector<Game> games(n);
  for (size_t i = 0; i < n; ++i) games[i].path = files.path(i);
  files = GameList();

  // sortnames are rare: kept on the side (index -> text) instead of a string in every Game
  std::unordered_map<uint32_t, std::string> sortnames;
  if (n && file_exists(gamelist)) {
    std::string pre = dir;
    if (!pre.empty() && pre.back() != '/') pre.push_back('/');
    auto rel_of_file = [&](const std::string& p) { return starts_with(p, pre) ? std::string_view(p).substr(pre.size()) : std::string_view(p); };
    // open-addressing table of game indices (+1) keyed by the path relative to the ROM folder: no per-game strings
    size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    std::vector<uint32_t> tab(cap, 0);
    for (size_t i = 0; i < n; ++i) {
      size_t h = size_t(hash_bytes(rel_of_file(games[i].path))) & (cap - 1);
      while (tab[h]) h = (h + 1) & (cap - 1);
      tab[h] = uint32_t(i + 1);
    }
    std::string scratch;
    auto on_entry = [&](const GameEntry& e) {
      std::string_view p = e.path, rel;
      if (starts_with(p, "./")) {
        rel = p.substr(2);
      } else if (!p.empty() && (p[0] == '/' || p[0] == '~')) {
        scratch = resolve_rel_path(p, dir);
        if (!starts_with(scratch, pre)) return;
        rel = std::string_view(scratch).substr(pre.size());
      } else {
        rel = p;
      }
      size_t h = size_t(hash_bytes(rel)) & (cap - 1);
      while (uint32_t v = tab[h]) {
        Game& g = games[v - 1];
        if (rel_of_file(g.path) == rel) {
          g.name.assign(e.name);
          g.meta = e.meta;
          if (e.sortname.empty()) sortnames.erase(v - 1);
          else sortnames[v - 1].assign(e.sortname);
          return;
        }
        h = (h + 1) & (cap - 1);
      }
    };
    gamelist_scan_file(gamelist, EntryFn::of(on_entry));
  }

  // names, hidden ones out
  size_t out = 0;
  std::vector<uint32_t> origin;  // original index of each kept game (to find its sortname)
  const bool have_sortnames = !sortnames.empty();
  if (have_sortnames) origin.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if (games[i].meta.hidden) continue;
    if (games[i].name.empty()) games[i].name = path_stem(games[i].path);
    if (have_sortnames) origin.push_back(uint32_t(i));
    if (out != i) games[out] = std::move(games[i]);
    ++out;
  }
  games.resize(out);

  // ES order: sort key = sortname or name, upper case, leading article dropped. Keys live in one buffer.
  std::string keys;
  keys.reserve(out * 24);
  std::vector<uint32_t> koff(out + 1);
  for (size_t i = 0; i < out; ++i) {
    koff[i] = uint32_t(keys.size());
    if (have_sortnames) {
      auto it = sortnames.find(origin[i]);
      if (it != sortnames.end()) {
        append_key(keys, it->second);
        continue;
      }
    }
    append_key(keys, games[i].name);
  }
  koff[out] = uint32_t(keys.size());
  std::vector<uint32_t> order(out);
  for (size_t i = 0; i < out; ++i) order[i] = uint32_t(i);
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return std::string_view(keys.data() + koff[a], koff[a + 1] - koff[a]) < std::string_view(keys.data() + koff[b], koff[b + 1] - koff[b]);
  });
  // apply the permutation in place: new[j] = old[order[j]]
  std::vector<char> done(out, 0);
  for (size_t i = 0; i < out; ++i) {
    if (done[i] || order[i] == i) continue;
    Game tmp = std::move(games[i]);
    size_t j = i;
    for (;;) {
      size_t k = order[j];
      done[j] = 1;
      if (k == i) {
        games[j] = std::move(tmp);
        break;
      }
      games[j] = std::move(games[k]);
      j = k;
    }
  }
  return games;
}

void System::open() {
  if (loaded || !games.empty()) return;
  games = load_games(rom_dir, exts, gamelist);
  count = games.size();
  loaded = true;
}

void System::apply_edit(const GamelistEdit& e) {
  const int64_t delta = int64_t(e.new_len) - int64_t(e.old_len);
  const uint64_t after = e.begin + e.old_len;
  for (Game& g : games) {
    GameMeta& m = g.meta;
    if (m.span_end == 0) continue;
    if (m.span_begin == e.begin && m.span_end == after && e.old_len) {
      m.span_end = uint32_t(int64_t(m.span_end) + delta);  // the edited element itself
    } else if (m.span_begin >= after) {
      m.span_begin = uint32_t(int64_t(m.span_begin) + delta);
      m.span_end = uint32_t(int64_t(m.span_end) + delta);
    }
  }
}

enum class LoadMode { Eager, Counted, Lazy };

static bool load_impl(const std::string& xml, const std::string& roms_root, LoadMode mode, bool keep_empty, std::vector<System>& out, std::string& err) {
  pugi::xml_document doc;
  pugi::xml_parse_result r = doc.load_buffer(xml.data(), xml.size());
  if (!r) {
    err = r.description();
    return false;
  }
  out.clear();
  // every <system> element at any depth, in document order
  std::vector<pugi::xml_node> nodes;
  collect_systems(doc, nodes);
  for (pugi::xml_node s : nodes) {
    std::string path = text_of(s, "path");
    constexpr std::string_view kRoms = "/storage/roms";
    if (!roms_root.empty() && starts_with(path, kRoms) && (path.size() == kRoms.size() || path[kRoms.size()] == '/')) path = roms_root + path.substr(kRoms.size());
    std::vector<std::string> exts;
    {
      std::string e = text_of(s, "extension");
      size_t i = 0;
      while (i < e.size()) {
        while (i < e.size() && (e[i] == ' ' || e[i] == '\t' || e[i] == '\n' || e[i] == '\r')) ++i;
        size_t j = i;
        while (j < e.size() && !(e[j] == ' ' || e[j] == '\t' || e[j] == '\n' || e[j] == '\r')) ++j;
        if (j > i) {
          std::string x = e.substr(i, j - i);
          for (char& c : x)
            if (c >= 'A' && c <= 'Z') c = char(c + 32);
          exts.push_back(x);
        }
        i = j;
      }
    }
    System sys;
    sys.name = text_of(s, "name");
    sys.gamelist = gamelist_for(sys.name, path);
    sys.rom_dir = path;
    sys.exts = exts;
    if (mode == LoadMode::Eager) {
      sys.games = load_games(path, exts, sys.gamelist);
      sys.count = sys.games.size();
      sys.loaded = true;
    } else if (mode == LoadMode::Counted) {
      sys.count = scan_count_games(path, Exts(exts));
    } else {
      if (scan_has_game(path, Exts(exts))) {  // stops at the first game
        sys.count = kCountPending;
      } else if (keep_empty) {
        sys.count = 0;
      } else {
        continue;
      }
    }
    if (sys.count == 0 && !keep_empty) continue;
    auto [emulator, core] = default_emu(s);
    sys.emulator = emulator;
    sys.core = core;
    std::string theme = text_of(s, "theme");
    sys.theme = theme.empty() ? sys.name : theme;
    sys.fullname = text_of(s, "fullname");
    sys.platform = text_of(s, "platform");
    sys.command = text_of(s, "command");
    sys.emus = all_emus(s);
    out.push_back(std::move(sys));
  }
  return true;
}

bool load_systems_text(const std::string& xml, const std::string& roms_root, bool eager, std::vector<System>& out, std::string& err, bool keep_empty) {
  return load_impl(xml, roms_root, eager ? LoadMode::Eager : LoadMode::Counted, keep_empty, out, err);
}

bool load_systems_text_lazy(const std::string& xml, const std::string& roms_root, std::vector<System>& out, std::string& err, bool keep_empty) {
  return load_impl(xml, roms_root, LoadMode::Lazy, keep_empty, out, err);
}

static bool load_cfg(const std::string& cfg, const std::string& roms_root, LoadMode mode, bool keep_empty, std::vector<System>& out, std::string& err) {
  std::string xml;
  if (!read_file(cfg, xml)) {
    err = cfg + ": No such file or directory";
    return false;
  }
  if (!load_impl(xml, roms_root, mode, keep_empty, out, err)) {
    err = cfg + ": " + err;
    return false;
  }
  return true;
}

bool load_systems(const std::string& cfg, const std::string& roms_root, std::vector<System>& out, std::string& err, bool keep_empty) {
  return load_cfg(cfg, roms_root, LoadMode::Eager, keep_empty, out, err);
}
bool load_systems_index(const std::string& cfg, const std::string& roms_root, std::vector<System>& out, std::string& err, bool keep_empty) {
  return load_cfg(cfg, roms_root, LoadMode::Counted, keep_empty, out, err);
}
bool load_systems_lazy(const std::string& cfg, const std::string& roms_root, std::vector<System>& out, std::string& err, bool keep_empty) {
  return load_cfg(cfg, roms_root, LoadMode::Lazy, keep_empty, out, err);
}

std::pair<std::string, std::string> resolve_emu(const System& sys, const Game& game, const ConfMap& conf) {
  // ES `FileData::getConfigurationName`: '=' and '#' are dropped from the file name (they cannot live in system.cfg keys)
  std::string file = path_filename(game.path);
  file.erase(std::remove_if(file.begin(), file.end(), [](char c) { return c == '=' || c == '#'; }), file.end());
  auto get = [&](const char* suffix) -> const std::string* {
    auto it = conf.find(sys.name + "[\"" + file + "\"]." + suffix);
    if (it != conf.end()) return &it->second;
    it = conf.find(sys.name + "." + suffix);
    return it == conf.end() ? nullptr : &it->second;
  };
  std::string emu = sys.emulator, core = sys.core;
  if (const std::string* e = get("emulator")) {
    auto it = std::find_if(sys.emus.begin(), sys.emus.end(), [&](const auto& x) { return x.first == *e; });
    if (it != sys.emus.end()) {
      if (*e != emu) core = it->second.empty() ? std::string() : it->second.front();
      emu = *e;
    }
  }
  if (const std::string* c = get("core")) {
    // accepted if ANY emulator entry named `emu` lists it
    for (const auto& x : sys.emus)
      if (x.first == emu && std::find(x.second.begin(), x.second.end(), *c) != x.second.end()) {
        core = *c;
        break;
      }
  }
  return {emu, core};
}

}  // namespace zf
