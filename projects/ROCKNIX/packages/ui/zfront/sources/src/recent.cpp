#include "recent.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <string_view>

#include "i18n.hpp"
#include "savestates.hpp"
#include "util.hpp"

namespace zf {

std::string ago_text(time_t then, time_t now) {
  if (then <= 0 || then > now) return std::string();
  long long s = (long long)(now - then);
  const char* fmt;
  long long n;
  if (s < 3600) {
    fmt = tr("%i min ago"), n = std::max(1LL, s / 60);
  } else if (s < 86400) {
    fmt = tr("%i h ago"), n = s / 3600;
  } else if (s < 30LL * 86400) {
    fmt = tr("%i d ago"), n = s / 86400;
  } else {
    return std::string();
  }
  std::string out = fmt;
  size_t p = out.find("%i");
  if (p != std::string::npos) out.replace(p, 2, std::to_string(n));
  return out;
}

std::string short_title(const std::string& stem) {
  size_t cut = stem.find(" (");
  size_t b = stem.find(" [");
  if (b != std::string::npos && (cut == std::string::npos || b < cut)) cut = b;
  if (cut == std::string::npos || cut == 0) return stem;
  return stem.substr(0, cut);
}

std::vector<RecentItem> recent_states(const System& sys, time_t now, size_t max) {
  struct Hit {
    std::string stem, shot;
    time_t t;
    int slot;
  };
  std::vector<Hit> hits;
  const std::string dir = savestates::state_dir(sys);
  DIR* d = ::opendir(dir.c_str());
  if (!d) return {};
  while (const dirent* e = ::readdir(d)) {
    std::string_view f = e->d_name;
    if (f.size() < 5 || f.substr(f.size() - 4) != ".png") continue;
    std::string stem;
    int slot = 0;
    if (!savestates::parse_name(f.substr(0, f.size() - 4), stem, slot)) continue;  // other screenshots in the folder
    std::string path = path_join(dir, std::string(f));
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || st.st_size <= 0) continue;
    auto it = std::find_if(hits.begin(), hits.end(), [&](const Hit& h) { return h.stem == stem; });
    if (it == hits.end()) {
      hits.push_back({std::move(stem), std::move(path), st.st_mtime, slot});
    } else if (st.st_mtime > it->t) {
      it->shot = std::move(path), it->t = st.st_mtime, it->slot = slot;  // the newest slot of that game
    }
  }
  ::closedir(d);
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.t > b.t; });
  if (hits.size() > max) hits.resize(max);
  std::vector<RecentItem> out;
  for (Hit& h : hits) out.push_back({std::move(h.shot), std::move(h.stem), ago_text(h.t, now), h.slot});
  return out;
}

}  // namespace zf
