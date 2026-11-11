#include "bezelproject.hpp"

#include <dirent.h>

#include <cctype>
#include <cstdlib>

#include "menu_game.hpp"
#include "util.hpp"

namespace zf::bezel {

std::string script() {
  const char* e = std::getenv("ZF_BEZEL_CMD");
  return e && *e ? shell_quote(e) : std::string("rocknix-es-thebezelproject");
}

static bool has_entries(const std::string& dir) {
  DIR* h = ::opendir(dir.c_str());
  if (!h) return false;
  bool any = false;
  while (const struct dirent* e = ::readdir(h)) {
    std::string n = e->d_name;
    if (n != "." && n != "..") {
      any = true;
      break;
    }
  }
  ::closedir(h);
  return any;
}

bool available() {
  if (!mg::bin_exists("rocknix-es-thebezelproject")) return false;
  for (const std::string& d : mg::env().bezel_dirs)
    if (has_entries(mg::env().rooted(d))) return true;
  return false;
}

bool valid_name(const std::string& n) {
  if (n.empty() || n.size() > 64) return false;
  for (char c : n)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')) return false;
  return true;
}

std::string list_command() { return script() + " list"; }
std::string install_command(const std::string& name) { return script() + " install " + name; }
std::string remove_command(const std::string& name) { return script() + " remove " + name; }

std::vector<Set> parse_list(const std::string& output) {
  std::vector<Set> res;
  size_t i = 0;
  while (i <= output.size()) {
    size_t e = output.find('\n', i);
    std::string line = output.substr(i, e == std::string::npos ? std::string::npos : e - i);
    i = e == std::string::npos ? output.size() + 1 : e + 1;
    std::vector<std::string> parts;  // splitAny(line, " \t"), empty tokens dropped
    std::string cur;
    for (char c : line) {
      if (c == ' ' || c == '\t' || c == '\r') {
        if (!cur.empty()) parts.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.size() < 2) continue;
    if (!starts_with(parts[0], "[I]") && !starts_with(parts[0], "[A]")) continue;
    Set s;
    s.installed = starts_with(parts[0], "[I]");
    s.name = parts[1];
    // ES: the url is the 3rd word, or the 4th when the 3rd is the "-" separator
    s.url = parts.size() < 3 ? std::string() : (parts[2] == "-" ? (parts.size() > 3 ? parts[3] : std::string()) : parts[2]);
    s.folder = parts.size() < 4 ? std::string() : parts[3];
    if (s.name != "?") res.push_back(std::move(s));
  }
  return res;
}

std::string last_line(const std::string& text) {
  size_t e = text.find_last_not_of(" \t\r\n");
  if (e == std::string::npos) return {};
  size_t b = text.find_last_of('\n', e);
  size_t from = b == std::string::npos ? 0 : b + 1;
  return text.substr(from, e - from + 1);
}

void on_installed(Ctx& cx, const std::string& system) {
  const std::string global = mg::conf_get(cx, "global.bezel");
  const std::string current = mg::conf_get(cx, system + ".bezel");
  if ((current.empty() || current == "AUTO") && !global.empty() && global != "AUTO") mg::conf_put(cx, system + ".bezel", "thebezelproject");
}

void on_removed(Ctx& cx, const std::string& system) {
  if (mg::conf_get(cx, system + ".bezel") == "thebezelproject") mg::conf_put(cx, system + ".bezel", "");
}

}  // namespace zf::bezel
