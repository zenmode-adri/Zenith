#include "library_doctor.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

#include "gamelist.hpp"
#include "util.hpp"

namespace zf::doctor {
namespace {

bool keep_tag(const std::string& inner) {
  // multi-disc sets differ only by this tag: they are not duplicates
  std::string t;
  for (char c : inner) t.push_back(char(std::tolower((unsigned char)c)));
  for (const char* k : {"disc", "disk", "side", "part", "cd"}) {
    const size_t n = std::strlen(k);
    if (t.compare(0, n, k) == 0 && t.size() > n && (std::isdigit((unsigned char)t[n]) || t[n] == ' ')) return true;
  }
  return false;
}

bool is_companion(const std::string& ext) { return ext == ".bin" || ext == ".img" || ext == ".sub" || ext == ".raw"; }
bool is_sheet(const std::string& ext) { return ext == ".cue" || ext == ".gdi" || ext == ".ccd" || ext == ".m3u"; }

// 0 = fine, 1 = empty, 2 = not the archive its extension says
int check_file(const std::string& path, const std::string& ext) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return 0;  // folders (m3u sets, ScummVM...) and vanished files are not judged
  if (st.st_size == 0) return 1;
  if (ext == ".zip" || ext == ".7z") {
    std::string head;
    if (!read_file_range(path, 0, 6, head)) return 0;  // unreadable: not a proof of anything
    if (ext == ".zip") return (head.size() >= 4 && head[0] == 'P' && head[1] == 'K' && (head[2] == 3 || head[2] == 5 || head[2] == 7)) ? 0 : 2;
    static const unsigned char k7[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
    return (head.size() >= 6 && std::memcmp(head.data(), k7, 6) == 0) ? 0 : 2;
  }
  return 0;
}

}  // namespace

std::string dup_stem(const std::string& file_name) {
  const std::string base = path_stem(file_name);
  std::string out;
  for (size_t i = 0; i < base.size(); ++i) {
    const char c = base[i];
    if (c == '(' || c == '[') {
      const size_t e = base.find(c == '(' ? ')' : ']', i + 1);
      if (e != std::string::npos) {
        if (keep_tag(base.substr(i + 1, e - i - 1)))
          for (size_t k = i; k <= e; ++k) out.push_back(char(std::tolower((unsigned char)base[k])));  // "(Disc 1)" kept, lower case like the rest
        i = e;
        continue;
      }
    }
    out.push_back(char(std::tolower((unsigned char)c)));
  }
  std::string r;  // collapse spaces
  for (char c : out) {
    if (c == ' ' && (r.empty() || r.back() == ' ')) continue;
    r.push_back(c);
  }
  while (!r.empty() && r.back() == ' ') r.pop_back();
  if (r.empty()) {
    for (char c : base) r.push_back(char(std::tolower((unsigned char)c)));
  }
  return r;
}

bool is_game_system(const System& sys) {
  if (sys.theme == "imageviewer") return false;
  for (const char* p : {"imageviewer", "ignore", "tools", "music"}) {  // es_systems.cfg platforms of what holds no games
    const std::string w = p;
    const std::string& pl = sys.platform;
    for (size_t i = pl.find(w); i != std::string::npos; i = pl.find(w, i + 1))
      if ((i == 0 || pl[i - 1] == ' ') && (i + w.size() == pl.size() || pl[i + w.size()] == ' ')) return false;
  }
  return true;
}

std::vector<std::pair<std::string, size_t>> bios_for(const std::vector<std::pair<std::string, size_t>>& all, const std::vector<std::string>& with_games) {
  std::vector<std::pair<std::string, size_t>> out;
  for (const auto& b : all)
    if (std::find(with_games.begin(), with_games.end(), b.first) != with_games.end()) out.push_back(b);
  return out;
}

Report scan(std::vector<System> systems) {
  Report rep;
  for (System& sys : systems) {
    if (!is_game_system(sys)) continue;  // screenshots, tools, music: no games to judge
    sys.open();
    if (!sys.games.empty()) {  // rocknix-systems names a system by its short name on the console ("scv"); its full name also matches
      rep.with_games.push_back(sys.name);
      if (!sys.fullname.empty()) rep.with_games.push_back(sys.fullname);
    }
    const std::string label = sys.fullname.empty() ? sys.name : sys.fullname;
    size_t missing = 0;
    struct Item {
      std::string file, ext;
    };
    std::map<std::string, std::vector<Item>> by_stem;
    for (const Game& g : sys.games) {
      ++rep.games;
      const std::string file = path_filename(g.path), ext = path_ext_lower(g.path);
      const int bad = check_file(g.path, ext);
      if (bad) rep.broken.push_back({label, file, bad == 1 ? "EMPTY FILE" : "NOT A VALID ARCHIVE"});
      by_stem[dup_stem(file)].push_back({file, ext});
      std::string img;
      if (g.meta.span_end != 0 && !sys.gamelist.empty()) img = gamelist_details(sys.gamelist, sys.rom_dir, g.path, g.meta).image;
      if (img.empty() || !file_exists(img)) ++missing;
    }
    if (missing) rep.no_image.push_back({label, missing});
    for (auto& kv : by_stem) {
      const std::vector<Item>& v = kv.second;
      if (v.size() < 2) continue;
      bool sheet = false;
      for (const Item& it : v) sheet = sheet || is_sheet(it.ext);
      size_t n = 0;
      for (const Item& it : v)
        if (!(sheet && is_companion(it.ext))) ++n;  // game.cue + game.bin is one game
      if (n >= 2) rep.duplicates.push_back({label, kv.first, std::to_string(n)});
    }
  }
  return rep;
}

}  // namespace zf::doctor
