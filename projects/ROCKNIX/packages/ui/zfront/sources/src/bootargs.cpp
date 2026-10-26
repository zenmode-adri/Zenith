#include "bootargs.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <vector>

#include "util.hpp"

namespace zf::boot {

namespace {
std::string lower(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = char(std::tolower(static_cast<unsigned char>(c)));
  return o;
}
bool usable(const std::string& dir) { return file_exists(dir + "/layout.xml"); }
}  // namespace

std::string resolve_theme(std::string_view theme_set, const std::string& root) {
  const std::string set = lower(theme_set);
  const bool zenith = set.find("zenith") != std::string::npos;
  const bool dark = set.find("dark") != std::string::npos;
  std::vector<std::string> cand;
  if (zenith) {
    cand.push_back(root + "/storage/.config/emulationstation/themes/" + std::string(theme_set));
    cand.push_back(root + "/usr/share/themes/" + std::string(theme_set));
  }
  cand.push_back(root + "/usr/share/zfront/theme-" + (dark ? "dark" : "light"));
  cand.push_back(root + "/usr/share/themes/es-theme-zenith" + (dark ? "-dark" : ""));
  for (const std::string& c : cand)
    if (usable(c)) return c;
  return {};
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

}  // namespace zf::boot
