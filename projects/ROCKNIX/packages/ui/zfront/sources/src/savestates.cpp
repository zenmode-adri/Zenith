#include "savestates.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

#include "util.hpp"

namespace zf::savestates {

namespace {

struct Rec {
  int slot;
  bool png;
};

struct Sig {
  ino_t ino = 0;
  int64_t sec = 0, nsec = 0;
  off_t size = 0;
  bool ok = false;
  bool operator==(const Sig& o) const { return ok && o.ok && ino == o.ino && sec == o.sec && nsec == o.nsec && size == o.size; }
};

struct DirCache {
  std::string dir;
  Sig sig;
  std::unordered_map<std::string, std::vector<Rec>> by_stem;
  bool valid = false;
};

DirCache g_cache;
Plan g_plan;
unsigned g_gen = 0;

Sig sig_of(const std::string& dir) {
  Sig s;
  struct stat st;
  if (::stat(dir.c_str(), &st) != 0) return s;
  s.ino = st.st_ino;
  s.sec = int64_t(st.st_mtim.tv_sec);
  s.nsec = int64_t(st.st_mtim.tv_nsec);
  s.size = st.st_size;
  s.ok = true;
  return s;
}

void scan_dir(const std::string& dir, std::unordered_map<std::string, std::vector<Rec>>& out) {
  out.clear();
  DIR* d = ::opendir(dir.c_str());
  if (!d) return;
  std::unordered_set<std::string> pngs;
  struct Pending {
    std::string name, stem;
    int slot;
  };
  std::vector<Pending> found;
  while (dirent* e = ::readdir(d)) {
    std::string_view f = e->d_name;
    if (f.empty() || f[0] == '.' || e->d_type == DT_DIR) continue;
    if (ends_with(f, ".png")) {
      if (f.find(".state") != std::string_view::npos) pngs.emplace(f);
      continue;
    }
    std::string stem;
    int slot = 0;
    if (parse_name(f, stem, slot)) found.push_back({std::string(f), std::move(stem), slot});
  }
  ::closedir(d);
  for (Pending& p : found) {
    bool png = pngs.count(p.name + ".png") != 0;
    out[p.stem].push_back({p.slot, png});
  }
  for (auto& kv : out) std::sort(kv.second.begin(), kv.second.end(), [](const Rec& a, const Rec& b) { return a.slot < b.slot; });
}

const std::vector<Rec>* recs_for(const std::string& dir, const std::string& stem) {
  Sig now = sig_of(dir);
  if (!g_cache.valid || g_cache.dir != dir || !(g_cache.sig == now)) {
    g_cache.dir = dir;
    g_cache.sig = now;
    g_cache.valid = true;
    scan_dir(dir, g_cache.by_stem);
  }
  auto it = g_cache.by_stem.find(stem);
  return it == g_cache.by_stem.end() ? nullptr : &it->second;
}

State make_state(const std::string& dir, const std::string& stem, const Rec& r) {
  State s;
  s.slot = r.slot;
  s.path = state_path(dir, stem, r.slot);
  if (r.png) s.shot = s.path + ".png";
  return s;
}

uint64_t fnv(uint64_t h, const unsigned char* p, size_t n) {
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

bool hash_file(const std::string& path, uint64_t& h) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  unsigned char buf[65536];
  h = 1469598103934665603ull;
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      ::close(fd);
      return false;
    }
    if (n == 0) break;
    h = fnv(h, buf, size_t(n));
  }
  ::close(fd);
  return true;
}

// copy through a temp file in the destination folder, then rename; `hash` (optional) gets the FNV-1a of the content
bool copy_file(const std::string& src, const std::string& dst, uint64_t* hash = nullptr) {
  int in = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) return false;
  const bool blocked = writes_blocked();  // preview / dry-run: read and hash, write to /dev/null, no rename
  std::string tmp = dst + ".zf-tmp";
  int out = ::open(blocked ? "/dev/null" : tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    ::close(in);
    return false;
  }
  unsigned char buf[65536];
  uint64_t h = 1469598103934665603ull;
  bool ok = true;
  for (;;) {
    ssize_t n = ::read(in, buf, sizeof buf);
    if (n < 0) {
      ok = false;
      break;
    }
    if (n == 0) break;
    h = fnv(h, buf, size_t(n));
    for (ssize_t off = 0; off < n;) {
      ssize_t w = ::write(out, buf + off, size_t(n - off));
      if (w <= 0) {
        ok = false;
        break;
      }
      off += w;
    }
    if (!ok) break;
  }
  ::close(in);
  ok = (::close(out) == 0) && ok;
  if (blocked) {
    if (hash && ok) *hash = h;
    return ok;
  }
  if (!ok || ::rename(tmp.c_str(), dst.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  if (hash) *hash = h;
  return true;
}

void remove_file(const std::string& p) {
  if (!writes_blocked()) ::unlink(p.c_str());
}
bool move_file(const std::string& a, const std::string& b) { return writes_blocked() || ::rename(a.c_str(), b.c_str()) == 0; }

std::string conf_name(const System& sys, const Game& g) {
  std::string f = path_filename(g.path);
  f.erase(std::remove_if(f.begin(), f.end(), [](char c) { return c == '=' || c == '#'; }), f.end());
  return sys.name + "[\"" + f + "\"]";
}

const std::string* conf_get(const ConfMap& conf, const std::string& k) {
  auto it = conf.find(k);
  return it == conf.end() ? nullptr : &it->second;
}

bool custom_config_present() {
  static const bool present = file_exists("/storage/.config/emulationstation/es_savestates.cfg") || file_exists("/usr/share/emulationstation/es_savestates.cfg");
  return present;
}

}  // namespace

bool parse_name(std::string_view f, std::string& stem, int& slot) {
  constexpr std::string_view kAuto = ".state.auto";
  if (ends_with(f, kAuto)) {
    if (f.size() == kAuto.size()) return false;
    stem.assign(f.substr(0, f.size() - kAuto.size()));
    slot = -1;
    return true;
  }
  size_t end = f.size(), d = end;
  while (d > 0 && f[d - 1] >= '0' && f[d - 1] <= '9') --d;
  constexpr std::string_view kState = ".state";
  if (d < kState.size() || f.substr(d - kState.size(), kState.size()) != kState) return false;
  size_t stem_len = d - kState.size();
  if (stem_len == 0) return false;
  std::string_view digits = f.substr(d);
  int v = 0;
  if (!digits.empty()) {
    if (digits.size() > 6) return false;
    for (char c : digits) v = v * 10 + (c - '0');
  }
  stem.assign(f.substr(0, stem_len));
  slot = v;
  return true;
}

std::string state_dir(const System& sys) {
  std::string rom = sys.rom_dir;
  while (rom.size() > 1 && rom.back() == '/') rom.pop_back();
  return path_join(path_join(path_dir(rom), "savestates"), sys.name);
}

std::string state_path(const std::string& dir, const std::string& stem, int slot) {
  if (slot < 0) return path_join(dir, stem + ".state.auto");
  return path_join(dir, stem + ".state" + (slot == 0 ? std::string() : std::to_string(slot)));
}

std::string setting(const ConfMap& conf, const System& sys, const Game& g, const std::string& key) {
  if (const std::string* v = conf_get(conf, conf_name(sys, g) + "." + key)) return *v;
  if (const std::string* v = conf_get(conf, sys.name + "." + key)) return *v;
  if (const std::string* v = conf_get(conf, "global." + key)) return *v;
  return {};
}

bool enabled(const System& sys, const Game& g, const ConfMap& conf) {
  if (custom_config_present()) return false;  // per-emulator layouts are not ported
  return resolve_emu(sys, g, conf).first == "retroarch";
}

bool incremental(const ConfMap& conf) {
  const std::string* v = conf_get(conf, "global.incrementalsavestates");
  return !v || (*v != "0" && *v != "2");
}

int next_free_slot(const std::vector<State>& states) {
  int best = -1;
  for (const State& s : states) best = std::max(best, s.slot);
  return best + 1;  // no state, or only the autosave: slot 0
}

std::vector<State> list(const System& sys, const Game& g) {
  std::vector<State> out;
  std::string dir = state_dir(sys), stem = path_stem(g.path);
  if (const std::vector<Rec>* r = recs_for(dir, stem))
    for (const Rec& x : *r) out.push_back(make_state(dir, stem, x));
  return out;
}

void with_times(std::vector<State>& v) {
  for (State& s : v) {
    struct stat st;
    s.mtime = ::stat(s.path.c_str(), &st) == 0 ? st.st_mtime : 0;
  }
}

void invalidate() {
  g_cache.valid = false;
  ++g_gen;
}

unsigned generation() { return g_gen; }

bool plan_active() { return g_plan.active; }

std::string plain_args(const System& sys, const Game& g, const ConfMap& conf) {
  if (!enabled(sys, g, conf)) return {};
  std::vector<State> st = list(sys, g);
  int next = next_free_slot(st);
  if (setting(conf, sys, g, "autosave") == "1") return " -autosave 1 -state_slot " + std::to_string(next);
  const std::string* gi = conf_get(conf, "global.incrementalsavestates");
  bool use_current = gi && *gi == "2";
  if (next > 0 && !use_current) return " -state_slot " + std::to_string(next);
  return {};
}

std::string plain_args(Ctx& cx, size_t sys, size_t game) {
  if (sys >= cx.systems.size() || game >= cx.systems[sys].games.size()) return {};
  return plain_args(cx.systems[sys], cx.systems[sys].games[game], cx.conf.map());
}

std::string begin(const System& sys, const Game& g, const ConfMap& conf, Pick pick, int slot) {
  if (!enabled(sys, g, conf)) return {};
  if (pick == Pick::None) return plain_args(sys, g, conf);
  std::string dir = state_dir(sys), stem = path_stem(g.path);
  std::vector<State> st = list(sys, g);
  int next = next_free_slot(st);
  if (pick == Pick::NewGame) return " -autosave 0 -state_slot " + std::to_string(next);
  if (pick == Pick::Auto) return " -autosave 1 -state_slot " + std::to_string(next);

  // a numbered state is loaded: RetroArch loads "<stem>.state.auto" at start (autosave 1), so the chosen state is copied there
  const State* chosen = nullptr;
  for (const State& s : st)
    if (s.slot == slot) chosen = &s;
  bool inc = incremental(conf);
  std::string args = " -state_slot " + std::to_string(inc ? next : slot);
  if (chosen) args += " -autosave 1";

  Plan plan;
  plan.active = true;
  plan.dir = dir;
  plan.stem = stem;
  plan.from_slot = true;
  plan.auto_file = state_path(dir, stem, -1);
  plan.auto_png = plan.auto_file + ".png";
  if (file_exists(plan.auto_file)) {
    remove_file(plan.auto_file + ".bak");
    plan.auto_saved = move_file(plan.auto_file, plan.auto_file + ".bak");
  }
  if (file_exists(plan.auto_png)) {
    remove_file(plan.auto_png + ".bak");
    plan.png_saved = move_file(plan.auto_png, plan.auto_png + ".bak");
  }
  if (chosen) {
    copy_file(chosen->path, plan.auto_file);
    if (inc && next >= 0 && slot + 1 != next) {
      plan.new_slot_file = state_path(dir, stem, next);
      remove_file(plan.new_slot_file);
      if (!copy_file(chosen->path, plan.new_slot_file, &plan.new_slot_hash)) plan.new_slot_file.clear();
    }
  }
  g_plan = std::move(plan);
  invalidate();
  return args;
}

void renumber(const std::string& dir, const std::string& stem) {
  invalidate();
  const std::vector<Rec>* r = recs_for(dir, stem);
  if (!r) return;
  std::vector<Rec> recs = *r;  // slots ascending
  int next = 0;
  for (const Rec& x : recs) {
    if (x.slot < 0) continue;
    if (x.slot != next) {
      std::string from = state_path(dir, stem, x.slot), to = state_path(dir, stem, next);
      move_file(from, to);
      if (x.png) move_file(from + ".png", to + ".png");
    }
    ++next;
  }
  invalidate();
}

void after_game() {
  if (!g_plan.active) return;
  Plan p = std::move(g_plan);
  g_plan = Plan();
  if (!p.new_slot_file.empty() && file_exists(p.new_slot_file)) {
    uint64_t h = 0;
    if (hash_file(p.new_slot_file, h) && h == p.new_slot_hash) remove_file(p.new_slot_file);  // the game did not touch it
  }
  // the autosave goes back to what it was before the loaded state was copied over it
  remove_file(p.auto_file);
  if (p.auto_saved) move_file(p.auto_file + ".bak", p.auto_file);
  remove_file(p.auto_png);
  if (p.png_saved) move_file(p.auto_png + ".bak", p.auto_png);
  if (p.from_slot) renumber(p.dir, p.stem);
  invalidate();
}

bool remove_state(const System& sys, const Game& g, const State& s) {
  if (s.path.empty()) return false;
  remove_file(s.path);
  if (!s.shot.empty()) remove_file(s.shot);
  renumber(state_dir(sys), path_stem(g.path));
  return true;
}

bool copy_to_free_slot(const System& sys, const Game& g, const State& s) {
  std::vector<State> all = list(sys, g);
  int slot = next_free_slot(all);
  if (slot < 0 || s.path.empty()) return false;
  std::string dir = state_dir(sys), stem = path_stem(g.path);
  std::string to = state_path(dir, stem, slot);
  if (!copy_file(s.path, to)) return false;
  if (!s.shot.empty()) copy_file(s.shot, to + ".png");
  invalidate();
  return true;
}

void recover(const System& sys, const Game& g) {
  if (g_plan.active) return;  // a game is about to run / running
  std::string dir = state_dir(sys), stem = path_stem(g.path);
  std::string a = state_path(dir, stem, -1);
  bool any = false;
  if (file_exists(a + ".bak")) {
    remove_file(a);
    move_file(a + ".bak", a);
    any = true;
  }
  if (file_exists(a + ".png.bak")) {
    remove_file(a + ".png");
    move_file(a + ".png.bak", a + ".png");
    any = true;
  }
  if (any) invalidate();
}

std::string make_arg(const System& sys, const Game& g) { return sys.name + "|" + g.path; }

bool parse_arg(Ctx& cx, const std::string& arg, size_t& sys, size_t& game) {
  size_t bar = arg.find('|');
  if (bar == std::string::npos) return false;
  std::string_view name = std::string_view(arg).substr(0, bar), path = std::string_view(arg).substr(bar + 1);
  for (size_t i = 0; i < cx.systems.size(); ++i) {
    if (cx.systems[i].name != name) continue;
    cx.open_system(i);
    const auto& games = cx.systems[i].games;
    for (size_t j = 0; j < games.size(); ++j)
      if (games[j].path == path) {
        sys = i;
        game = j;
        return true;
      }
  }
  return false;
}

Transition launch_request(Ctx& cx, size_t sys, size_t game) {
  if (sys >= cx.systems.size() || game >= cx.systems[sys].games.size()) return Transition::none();
  const System& s = cx.systems[sys];
  const Game& g = s.games[game];
  if (!enabled(s, g, cx.conf.map())) return Transition::none();
  std::string mode = setting(cx.conf.map(), s, g, "savestates");
  bool show = mode == "1" || (mode == "2" && !list(s, g).empty());
  if (!show) return Transition::none();
  return Transition::open("menu.savestates", make_arg(s, g));
}

Transition open_manager(Ctx& cx, size_t sys, size_t game) {
  if (sys >= cx.systems.size() || game >= cx.systems[sys].games.size()) return Transition::none();
  const System& s = cx.systems[sys];
  const Game& g = s.games[game];
  if (!enabled(s, g, cx.conf.map())) return Transition::none();
  return Transition::open("menu.savestates", make_arg(s, g));
}

}  // namespace zf::savestates
