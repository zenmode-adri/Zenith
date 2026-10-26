// menu-game: sources of the lists the menus offer (shaders, filters, governors, bezels), system.cfg access and the
// helpers that work out which emulator/core a config means. Pure functions apart from the caches and `conf_put`.
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>

#include "menu_game.hpp"
#include "util.hpp"

namespace zf::mg {

Env& env() {
  static Env e = [] {
    Env x;
    if (const char* r = std::getenv("ZF_MG_ROOT")) x.root = r;
    return x;
  }();
  return e;
}

// ---- caches ----------------------------------------------------------------------------------------------------------
namespace {

struct ListCache {
  std::mutex m;
  std::vector<std::string> v;
  std::atomic<bool> loaded{false};
};
ListCache& shader_cache() {
  static ListCache c;
  return c;
}
ListCache& filter_cache() {
  static ListCache c;
  return c;
}

bool is_directory_entry(const std::string& dir, const struct dirent* e) {
#ifdef _DIRENT_HAVE_D_TYPE
  if (e->d_type == DT_DIR) return true;
  if (e->d_type != DT_UNKNOWN) return false;
#endif
  struct stat st;
  std::string p = dir + "/" + e->d_name;
  return ::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/// `find <root> -name/-regex` for files whose name ends with one of `exts`; paths relative to `root`.
void walk(const std::string& root, const std::string& rel, const std::vector<std::string>& exts, std::vector<std::string>& out, int depth) {
  if (depth > 16) return;
  std::string dir = rel.empty() ? root : root + "/" + rel;
  DIR* d = ::opendir(dir.c_str());
  if (!d) return;
  while (const struct dirent* e = ::readdir(d)) {
    std::string_view n = e->d_name;
    if (n == "." || n == "..") continue;
    std::string r = rel.empty() ? std::string(n) : rel + "/" + std::string(n);
    if (is_directory_entry(dir, e)) {
      walk(root, r, exts, out, depth + 1);
      continue;
    }
    for (const std::string& x : exts)
      if (ends_with(n, x)) {
        out.push_back(r);
        break;
      }
  }
  ::closedir(d);
}

/// Extensions `getshaders` looks for: `$UI_SHADER` (`glslp`, or `glslp\|slangp`), else the profile file the quirks write,
/// else glslp (RK3326).
std::vector<std::string> shader_exts() {
  std::string v;
  if (const char* e = std::getenv("UI_SHADER")) v = e;
  if (v.empty()) {
    std::string t;
    if (read_file(env().rooted(env().profile_shader), t)) {
      size_t p = t.find("UI_SHADER=");
      if (p != std::string::npos) {
        size_t e = t.find('\n', p);
        v = t.substr(p + 10, e == std::string::npos ? std::string::npos : e - p - 10);
        std::string clean;
        for (char c : v)
          if (c != '"' && c != '\'' && c != '\r') clean += c;
        v = clean;
      }
    }
  }
  if (v.empty()) v = "glslp";
  std::vector<std::string> exts;
  std::string cur;
  auto flush = [&] {
    if (!cur.empty()) exts.push_back("." + cur);
    cur.clear();
  };
  for (char c : v) {
    if (c == '|' || c == '\\') flush();
    else cur += c;
  }
  flush();
  return exts;
}

std::vector<std::string> read_words(const std::string& path) {
  std::string t;
  std::vector<std::string> out;
  if (!read_file(path, t)) return out;
  std::string cur;
  for (char c : t + " ") {
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
      bool alpha = false;
      for (char x : cur)
        if (x >= 'a' && x <= 'z') alpha = true;
      if (alpha) out.push_back(cur);  // `grep [a-z]`
      cur.clear();
    } else {
      cur += c;
    }
  }
  return out;
}

}  // namespace

void reset_caches() {
  reset_features();
  for (ListCache* c : {&shader_cache(), &filter_cache()}) {
    std::lock_guard<std::mutex> lk(c->m);
    c->v.clear();
    c->loaded = false;
  }
}

const std::vector<std::string>& shader_list(bool* loaded) {
  static const std::vector<std::string> kEmpty;
  ListCache& c = shader_cache();
  bool l = c.loaded.load();
  if (loaded) *loaded = l;
  return l ? c.v : kEmpty;
}

void load_shader_list() {
  ListCache& c = shader_cache();
  std::vector<std::string> v;
  walk(env().rooted(env().shaders_dir), "", shader_exts(), v, 0);
  std::sort(v.begin(), v.end());
  std::lock_guard<std::mutex> lk(c.m);
  c.v = std::move(v);
  c.loaded = true;
}

const std::vector<std::string>& filter_list(bool* loaded) {
  static const std::vector<std::string> kEmpty;
  ListCache& c = filter_cache();
  bool l = c.loaded.load();
  if (loaded) *loaded = l;
  return l ? c.v : kEmpty;
}

void load_filter_list() {
  ListCache& c = filter_cache();
  std::vector<std::string> v;
  walk(env().rooted(env().filters_dir), "", {".filt"}, v, 0);
  std::sort(v.begin(), v.end());
  std::lock_guard<std::mutex> lk(c.m);
  c.v = std::move(v);
  c.loaded = true;
}

std::vector<std::string> cpu_governors() {
  std::vector<std::string> v = {"default"};
  for (std::string& g : read_words(env().rooted(env().cpu_governors))) v.push_back(std::move(g));
  return v;
}

std::vector<std::string> gpu_governors() {
  std::string base = env().gpu_freq_default;
  std::string t;
  if (read_file(env().rooted(env().profile_governors), t)) {
    size_t p = t.find("GPU_FREQ=");
    if (p != std::string::npos) {
      size_t e = t.find('\n', p);
      std::string line = t.substr(p + 9, e == std::string::npos ? std::string::npos : e - p - 9);
      // GPU_FREQ=("/sys/...") or GPU_FREQ="/sys/..."
      size_t a = line.find('/');
      size_t b = a == std::string::npos ? a : line.find_first_of("\")' \r", a);
      if (a != std::string::npos) base = line.substr(a, b == std::string::npos ? std::string::npos : b - a);
    }
  }
  std::vector<std::string> v = {"default"};
  for (std::string& g : read_words(env().rooted(base + "/available_governors"))) v.push_back(std::move(g));
  return v;
}

std::vector<std::string> decoration_sets() {
  std::vector<std::string> out;
  for (const std::string& d : env().bezel_dirs) {
    std::string dir = env().rooted(d);
    DIR* h = ::opendir(dir.c_str());
    if (!h) continue;
    while (const struct dirent* e = ::readdir(h)) {
      std::string_view n = e->d_name;
      if (n == "." || n == "..") continue;
      struct stat st;
      std::string p = dir + "/" + std::string(n);
      if (::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) out.emplace_back(n);
    }
    ::closedir(h);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool bin_exists(const char* name) { return file_exists(env().rooted(env().bin_dir + "/" + name)); }

const std::vector<std::pair<const char*, const char*>>& ratios() {
  // The libretro list of ES (LibretroRatio), in a readable order: AUTO, the numeric ratios by shape, then the special ones.
  static const std::vector<std::pair<const char*, const char*>> r = {
      {N_("AUTO"), "auto"},   {"4/3", "4/3"},     {"16/9", "16/9"},   {"16/10", "16/10"}, {"16/15", "16/15"}, {"21/9", "21/9"},   {"1/1", "1/1"},
      {"2/1", "2/1"},     {"3/2", "3/2"},     {"3/4", "3/4"},     {"4/1", "4/1"},     {"9/16", "9/16"},   {"5/4", "5/4"},     {"6/5", "6/5"},
      {"7/9", "7/9"},     {"8/3", "8/3"},     {"8/7", "8/7"},     {"19/12", "19/12"}, {"19/14", "19/14"}, {"30/17", "30/17"}, {"32/9", "32/9"},
      {N_("CONFIG"), "config"}, {N_("SQUARE PIXEL"), "squarepixel"}, {N_("CORE PROVIDED"), "core"}, {N_("CUSTOM"), "custom"}, {N_("FULL"), "full"}};
  return r;
}

// ---- system.cfg ------------------------------------------------------------------------------------------------------
std::string conf_get(const Ctx& cx, const std::string& key) { return cx.conf.get_or(key, std::string()); }

bool conf_put(Ctx& cx, const std::string& key, const std::string& value) {
  bool remove = value.empty() || value == "auto" || value == "default";
  const std::string* cur = cx.conf.get(key);
  if (remove ? cur == nullptr : (cur && *cur == value)) return true;  // nothing to change: no write, no new mtime
  std::string err;
  bool ok = remove ? cx.conf.persist_remove(key, &err) : cx.conf.persist(key, value, &err);
  if (!ok) cx.sv->toasts.push(tr("Could not save the setting"), 3000);
  return ok;
}

std::string game_cfg(const std::string& system, std::string_view rom_file) {
  std::string f;
  for (char c : rom_file)
    if (c != '=' && c != '#') f += c;
  return system + "[\"" + f + "\"]";
}

std::string game_arg(std::string_view system, std::string_view rom_path) {
  std::string a(system);
  a += '\t';
  a += rom_path;
  return a;
}

bool split_game_arg(std::string_view arg, std::string& system, std::string& rom_path) {
  size_t t = arg.find('\t');
  if (t == std::string_view::npos || t == 0 || t + 1 >= arg.size()) return false;
  system.assign(arg.substr(0, t));
  rom_path.assign(arg.substr(t + 1));
  return true;
}

// ---- systems ---------------------------------------------------------------------------------------------------------
const System* find_system(const Ctx& cx, const std::string& name) {
  for (const System& s : cx.systems)
    if (s.name == name) return &s;
  return nullptr;
}

bool has_emulator_selection(const System& sys) {
  size_t cores = 0;
  for (const auto& e : sys.emus) cores += e.second.size();
  return sys.emus.size() > 1 || cores > 1;
}

namespace {

const std::vector<std::string>* cores_of(const System& sys, const std::string& emu) {
  for (const auto& e : sys.emus)
    if (e.first == emu) return &e.second;
  return nullptr;
}

}  // namespace

Pick explicit_pick(const System& sys, const ConfMap& conf, const std::string& cfg) {
  auto get = [&](const char* k) -> std::string {
    auto it = conf.find(cfg + "." + k);
    return it == conf.end() ? std::string() : it->second;
  };
  Pick p;
  std::string e = get("emulator"), c = get("core");
  if (e == "auto") e.clear();
  if (c == "auto") c.clear();
  if (!e.empty() && !cores_of(sys, e)) e.clear();  // ES: unknown emulator = not set
  // the core is checked against the emulator that would run
  std::string eff = e;
  if (eff.empty()) {
    auto it = conf.find(sys.name + ".emulator");
    if (it != conf.end() && cores_of(sys, it->second)) eff = it->second;
    else eff = sys.emulator;
  }
  if (!c.empty()) {
    const std::vector<std::string>* cs = cores_of(sys, eff);
    if (!cs || std::find(cs->begin(), cs->end(), c) == cs->end()) c.clear();
  }
  p.emu = e;
  p.core = c;
  return p;
}

Pick resolved_pick(const System& sys, const ConfMap& conf, std::string_view rom_file) {
  Game g;
  g.path = std::string(rom_file.empty() ? "" : rom_file);
  auto [e, c] = resolve_emu(sys, g, conf);
  return Pick{e, c};
}

}  // namespace zf::mg
