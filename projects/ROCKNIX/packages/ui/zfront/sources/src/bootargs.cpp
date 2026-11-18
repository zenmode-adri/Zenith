#include "bootargs.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <vector>

#include "es_theme.hpp"
#include "theme.hpp"
#include "util.hpp"

namespace zf::boot {

const std::vector<UserThemeKey>& user_theme_keys() {
  static const std::vector<UserThemeKey> k = {
      {"zfront.sel_style", "selStyle", {"fill", "outline", "underline", "bar"}},
      {"zfront.header_align", "headerAlign", {"left", "center"}},
      {"zfront.list_case", "listCase", {"upper", "lower"}},
  };
  return k;
}

void apply_user_theme(Theme& th, const std::function<std::string(const std::string&)>& get) {
  for (const UserThemeKey& k : user_theme_keys()) {
    const std::string v = get(k.key);
    if (std::find(k.values.begin(), k.values.end(), v) != k.values.end()) th.set_var(k.var, v);
  }
}

void set_user_theme(Theme& th, const std::string& key, const std::string& value) {
  for (const UserThemeKey& k : user_theme_keys()) {
    if (k.key != key) continue;
    if (value.empty()) th.reset_var(k.var);
    else if (std::find(k.values.begin(), k.values.end(), value) != k.values.end()) th.set_var(k.var, value);
  }
}

namespace {
std::string lower(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = char(std::tolower(static_cast<unsigned char>(c)));
  return o;
}
bool usable(const std::string& dir) { return file_exists(dir + "/layout.xml"); }

std::vector<std::string> list_dir(const std::string& dir) {
  std::vector<std::string> v;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (dirent* e = ::readdir(d))
      if (e->d_name[0] != '.') v.emplace_back(e->d_name);
    ::closedir(d);
  }
  std::sort(v.begin(), v.end());
  return v;
}
}  // namespace

std::string resolve_theme(std::string_view theme_set_in, const std::string& root) {
  // no ThemeSet (a new console, a cleared setting): Zenith Next dark, the image's default theme
  const std::string_view theme_set = theme_set_in.empty() ? std::string_view(kDefaultThemeSet) : theme_set_in;
  const std::string set = lower(theme_set);
  const bool zenith = set.find("zenith") != std::string::npos;
  const bool dark = set.find("dark") != std::string::npos;
  std::vector<std::string> cand;
  if (zenith) {
    cand.push_back(root + "/storage/.config/emulationstation/themes/" + std::string(theme_set));
    cand.push_back(root + "/usr/share/themes/" + std::string(theme_set));
    // colour variants shipped with zfront: ThemeSet `es-theme-zenith-<name>` is `/usr/share/zfront/theme-<name>`
    const size_t z = set.rfind("zenith-");
    if (z != std::string::npos && set.size() > z + 7 && set.compare(z + 7, std::string::npos, "dark") != 0)
      cand.push_back(root + "/usr/share/zfront/theme-" + set.substr(z + 7));
  }
  cand.push_back(root + "/usr/share/zfront/theme-" + (dark ? "dark" : "light"));
  cand.push_back(root + "/usr/share/themes/es-theme-zenith" + (dark ? "-dark" : ""));
  for (const std::string& c : cand)
    if (usable(c)) return c;
  return {};
}

namespace {
const char* const kEsThemeDirs[] = {"/storage/.config/emulationstation/themes", "/usr/share/themes", "/etc/emulationstation/themes", "/usr/share/emulationstation/themes"};
// a folder that is an ES theme and not one of ours: theme.xml but no Zenith layout.xml
bool is_es_theme(const std::string& dir, const std::string& name) { return lower(name).find("zenith") == std::string::npos && file_exists(dir + "/theme.xml") && !usable(dir); }
void add_unique(std::vector<ThemeChoice>& out, const ThemeChoice& t) {
  for (const ThemeChoice& o : out)
    if (o.value == t.value) return;
  out.push_back(t);
}
}  // namespace

namespace {
struct VirtualTheme {
  const char* value;
  const char* label;
  const char* carousel;
};
const VirtualTheme kVirtual[] = {
    {"zf-industrial", "Industrial", "industrial"},
    {"zf-swiss", "Swiss", "swiss"},
    {"zf-aurora", "Aurora", "aurora"},
};
const char* const kVirtualBase = "es-theme-iconic";  // the ES theme whose pictures they use
}  // namespace

std::string virtual_carousel(std::string_view set) {
  for (const VirtualTheme& v : kVirtual)
    if (set == v.value) return v.carousel;
  return {};
}

std::string resolve_es_theme(std::string_view theme_set, const std::string& root) {
  if (theme_set.empty() || theme_set.find('/') != std::string_view::npos || theme_set[0] == '.') return {};
  if (!virtual_carousel(theme_set).empty()) return resolve_es_theme(kVirtualBase, root);
  for (const char* d : kEsThemeDirs) {
    const std::string dir = root + d + "/" + std::string(theme_set);
    if (is_dir(dir) && is_es_theme(dir, std::string(theme_set))) return dir;
  }
  return {};
}

std::vector<ThemeChoice> list_es_themes(const std::string& root) {
  std::vector<ThemeChoice> out;
  for (const char* d : kEsThemeDirs) {
    const std::string base = root + d;
    for (const std::string& n : list_dir(base)) {
      if (!is_dir(base + "/" + n) || !is_es_theme(base + "/" + n, n)) continue;
      std::string label = n;
      if (label.rfind("es-theme-", 0) == 0) label.erase(0, 9);
      for (char& c : label)
        if (c == '-' || c == '_') c = ' ';
      if (!label.empty()) label[0] = char(std::toupper(static_cast<unsigned char>(label[0])));
      add_unique(out, {n, label + " (ES)", true});
    }
  }
  return out;
}

bool translate_es_theme(const std::string& es_dir, unsigned w, unsigned h, const std::function<const std::string*(const std::string&)>& setting,
                        const std::string& root, const std::string& out_root, std::string& dir, std::string& report, std::string& err) {
  estheme::Options opt;
  opt.ratio = ratio_name(w, h);
  opt.width = w;
  opt.height = h;
  opt.setting = setting;
  if (setting)
    if (const std::string* ts = setting("ThemeSet")) opt.carousel = virtual_carousel(*ts);
  estheme::Bases bases{resolve_theme("", root), resolve_theme("dark", root)};
  estheme::Generated g;
  if (!estheme::generate(es_dir, bases, out_root.empty() ? estheme::default_out_root() : out_root, opt, g, err)) return false;
  dir = g.dir;
  report = g.report_text;
  return true;
}

std::vector<ThemeChoice> list_theme_sets(const std::string& root) {
  auto pretty = [](std::string v) {
    if (v == "es-theme-zenith") return std::string("Zenith Minimal");
    if (v == "es-theme-zenith-dark") return std::string("Zenith Minimal Dark");
    if (v == "es-theme-zenith-next") return std::string("Zenith Next");
    if (v == "es-theme-zenith-next-dark") return std::string("Zenith Next Dark");
    if (v.rfind("es-theme-", 0) == 0) v.erase(0, 9);
    for (char& c : v)
      if (c == '-' || c == '_') c = ' ';
    if (!v.empty()) v[0] = char(std::toupper(static_cast<unsigned char>(v[0])));
    return v;
  };
  std::vector<ThemeChoice> out;
  auto add = [&](const std::string& value) {
    for (const ThemeChoice& t : out)
      if (t.value == value) return;
    out.push_back({value, pretty(value)});
  };
  auto scan = [&](const std::string& dir, bool bundled) {
    for (const std::string& n : list_dir(dir)) {
      const std::string l = lower(n);
      if (bundled) {  // theme-light, theme-dark, theme-<variant>
        if (l.rfind("theme-", 0) != 0 || !usable(dir + "/" + n)) continue;
        const std::string v = l.substr(6);
        add(v == "light" ? "es-theme-zenith" : "es-theme-zenith-" + v);
      } else if (l.find("zenith") != std::string::npos && usable(dir + "/" + n)) {
        add(n);
      }
    }
  };
  scan(root + "/usr/share/zfront", true);
  scan(root + "/usr/share/themes", false);
  scan(root + "/storage/.config/emulationstation/themes", false);
  for (const ThemeChoice& t : list_es_themes(root)) add_unique(out, t);
  if (!resolve_es_theme(kVirtualBase, root).empty())
    for (const VirtualTheme& v : kVirtual) add_unique(out, {v.value, v.label, true});
  std::sort(out.begin(), out.end(), [](const ThemeChoice& a, const ThemeChoice& b) {
    auto rank = [](const ThemeChoice& t) { return t.value == "es-theme-zenith" ? 0 : lower(t.value).find("zenith") != std::string::npos ? 1 : 2; };  // Zenith Minimal, the other Zeniths, the rest
    if (rank(a) != rank(b)) return rank(a) < rank(b);
    return a.label < b.label;
  });
  return out;
}

std::string theme_family(std::string_view set) {
  constexpr std::string_view kDark = "-dark";
  if (set.size() > kDark.size() && lower(set.substr(set.size() - kDark.size())) == kDark) set.remove_suffix(kDark.size());
  return std::string(set);
}

bool theme_is_dark(std::string_view set) { return theme_family(set).size() != set.size(); }

std::vector<ThemeChoice> theme_families(const std::vector<ThemeChoice>& sets) {
  auto has = [&](const std::string& v) {
    for (const ThemeChoice& t : sets)
      if (t.value == v) return true;
    return false;
  };
  std::vector<ThemeChoice> out;
  for (const ThemeChoice& t : sets) {
    if (theme_is_dark(t.value) && has(theme_family(t.value))) continue;  // its light sibling stands for both
    out.push_back(t);
  }
  return out;
}

std::string theme_in_mode(std::string_view family, bool dark, const std::vector<ThemeChoice>& sets) {
  const std::string want = dark ? std::string(family) + "-dark" : std::string(family);
  for (const ThemeChoice& t : sets)
    if (t.value == want) return want;
  return {};
}

bool pick_theme(const std::string& primary, const std::string& fallback, const std::function<bool(const std::string&, std::string&)>& problem,
                std::string& used, bool& fell_back, std::string& why) {
  fell_back = false;
  why.clear();
  if (!problem(primary, why)) {
    used = primary;
    return true;
  }
  std::string ignore;
  if (fallback.empty() || fallback == primary || problem(fallback, ignore)) return false;
  used = fallback;
  fell_back = true;
  return true;
}

bool redirect_log(const std::string& dir) {
  struct stat sb;
  if (dir.empty() || ::stat(dir.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) return false;
  const std::string path = dir + "/zfront.log";
  const bool big = ::stat(path.c_str(), &sb) == 0 && sb.st_size > (1 << 20);
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | (big ? O_TRUNC : 0), 0644);
  if (fd < 0) return false;
  std::fflush(stderr);
  bool ok = ::dup2(fd, 2) == 2;
  ::close(fd);
  return ok;
}

std::string startup_marker() { return "/run/zfront/startup-game"; }

bool startup_game_allowed(bool preview, bool dry_run, bool dump, bool harness) { return !preview && !dry_run && !dump && !harness; }

std::string startup_game(const std::string& rom, const std::string& marker, const std::function<bool(const std::string&)>& exists, std::string& why) {
  why.clear();
  if (rom.empty()) return {};
  if (!exists(rom)) {
    why = "the game set to start with the console is not there: " + rom;
    return {};
  }
  struct stat st;
  if (::stat(marker.c_str(), &st) == 0) return {};  // this boot started it already
  for (size_t p = 1; p < marker.size(); ++p)  // its folders, outermost first (EEXIST is fine)
    if (marker[p] == '/') ::mkdir(marker.substr(0, p).c_str(), 0755);
  int fd = ::open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) {
    if (errno != EEXIST) why = "cannot write " + marker + ", so the game at startup does not start: " + std::strerror(errno);
    return {};
  }
  ::close(fd);
  return rom;
}

}  // namespace zf::boot
