#include "moonlight.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <random>

#include <pugixml.hpp>

#include "launch.hpp"
#include "settings.hpp"
#include "util.hpp"

namespace zf::moonlight {

Paths& paths() {
  static Paths p;
  return p;
}

bool qt_build() { return file_exists(paths().rooted(paths().conf_qt)); }

std::string cert_dir() { return paths().rooted(qt_build() ? paths().cache_qt : paths().cache_embedded) + "/"; }

bool is_embedded(const std::string& out) { return out.find("Embedded") != std::string::npos; }

bool valid_host(const std::string& ip) { return launch::valid_netplay_host(ip); }

std::string quit_command(const std::string& ip) { return "moonlight quit " + ip; }

std::string pair_command(const std::string& pin, const std::string& ip, bool embedded) {
  return std::string(embedded ? "" : "QT_QPA_PLATFORM=wayland ") + "moonlight pair -pin " + pin + " " + ip;
}

bool parse_server_ip(const std::string& line, std::string* ip) {
  const std::string prompt = "Connect to ";
  if (line.compare(0, prompt.size(), prompt) != 0) return false;
  std::string rest = line.substr(prompt.size());
  while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ')) rest.pop_back();
  if (rest.size() < 3 || rest.compare(rest.size() - 3, 3, "...") != 0) return false;
  *ip = rest.substr(0, rest.size() - 3);
  return true;
}

bool paired_ok(const std::string& output) {
  size_t i = 0;
  while (i <= output.size()) {
    size_t e = output.find('\n', i);
    std::string line = output.substr(i, e == std::string::npos ? std::string::npos : e - i);
    while (!line.empty() && line.back() == '\r') line.pop_back();
    if (line == "Succesfully paired") return true;  // ES's spelling
    if (e == std::string::npos) break;
    i = e + 1;
  }
  return false;
}

bool parse_applist(const std::string& xml, std::vector<App>& out) {
  out.clear();
  if (xml.empty()) return false;
  pugi::xml_document doc;
  if (!doc.load_string(xml.c_str())) return false;
  pugi::xml_node root = doc.child("root");
  if (!root) return false;
  for (pugi::xml_node app : root.children()) {
    if (std::string(app.name()) != "App") continue;
    out.push_back({app.child("AppTitle").text().get(), app.child("ID").text().get()});
  }
  return true;
}

static std::string clean(const std::string& s) {
  std::string o = s;
  for (char& c : o)
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
  return o;
}

std::string script_name(const std::string& title) {
  std::string f = clean(title);
  std::replace(f.begin(), f.end(), '/', ' ');
  return f;
}

// inside "..." of sh: these four keep a meaning
static std::string quoted(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '\\' || c == '"' || c == '$' || c == '`') o += '\\';
    o += c;
  }
  return o + "\"";
}

std::string script_text(const std::string& ip, const std::string& title, bool qt) {
  const std::string name = script_name(title), t = clean(title);
  std::string s = "#!/bin/sh\n";
  s += "/usr/bin/controller-layout moonlight " + quoted(name + ".sh") + " " + paths().layout_db + "\n";
  if (qt) s += "QT_QPA_PLATFORM=wayland moonlight stream " + ip + " " + quoted(t) + " --quit-after\n";
  else s += "moonlight stream -app " + quoted(t) + " -platform sdl " + ip + "\n";
  return s;
}

namespace {
struct StringWriter : pugi::xml_writer {
  std::string out;
  void write(const void* data, size_t size) override { out.append(static_cast<const char*>(data), size); }
};
}  // namespace

std::string gamelist_text(const std::vector<App>& apps, const std::string& old_xml) {
  pugi::xml_document doc, old;
  if (!old_xml.empty()) old.load_buffer(old_xml.data(), old_xml.size());
  pugi::xml_node list = doc.append_child("gameList");
  for (const App& a : apps) {
    const std::string f = script_name(a.title), path = "./" + f + ".sh";
    pugi::xml_node g = list.append_child("game");
    g.append_child("path").text().set(path.c_str());
    g.append_child("name").text().set(a.title.c_str());
    g.append_child("image").text().set(("./images/" + f + ".png").c_str());
    // favourite, play count, last played, hidden...: the user's, not the server's
    for (pugi::xml_node og : old.child("gameList").children("game")) {
      if (path != og.child("path").text().get()) continue;
      for (pugi::xml_node c : og.children()) {
        const std::string n = c.name();
        if (n != "path" && n != "name" && n != "image") g.append_copy(c);
      }
      break;
    }
  }
  StringWriter w;
  doc.save(w);
  return w.out;
}

std::string extract_pem(const std::string& conf, const std::string& key) {
  const std::string head = key + "=";
  size_t i = 0;
  while (i < conf.size()) {
    size_t e = conf.find('\n', i);
    std::string line = conf.substr(i, e == std::string::npos ? std::string::npos : e - i);
    i = e == std::string::npos ? conf.size() : e + 1;
    while (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.compare(0, head.size(), head) != 0) continue;
    std::string v = line.substr(head.size());
    if (!v.empty() && v.front() == '"') v.erase(0, 1);
    if (!v.empty() && v.back() == '"') v.pop_back();
    const std::string ba = "@ByteArray(";
    if (v.compare(0, ba.size(), ba) != 0) return std::string();
    v.erase(0, ba.size());
    if (!v.empty() && v.back() == ')') v.pop_back();
    std::string o;
    for (size_t k = 0; k < v.size(); ++k) {
      if (v[k] == '\\' && k + 1 < v.size() && v[k + 1] == 'n') {
        o += '\n';
        ++k;
      } else {
        o += v[k];
      }
    }
    return o;
  }
  return std::string();
}

std::string new_guid() {
  static std::mt19937 rng(unsigned(std::time(nullptr)) ^ (unsigned(::getpid()) << 16));
  auto r = [] { return unsigned(rng() & 0x7fffffff); };
  char buf[128];
  std::snprintf(buf, sizeof buf, "%x%x-%x-%x-%x-%x%x%x", r(), r(), r(), ((r() & 0x0fff) | 0x4000), r() % 0x3fff + 0x8000, r(), r(), r());
  return buf;
}

std::string make_url(const std::string& ip, const std::string& command, std::string args, const std::string& guid) {
  if (!args.empty()) args += "&";
  args += "uniqueid=0123456789ABCDEF&uuid=" + guid;
  return "https://" + ip + ":47984/" + command + "?" + args;
}

std::string request_command(const std::string& curl, const std::string& url, const std::string& out_file) {
  const std::string dir = cert_dir();
  return shell_quote(curl) + " -s -f -k -m 20 --cert " + shell_quote(dir + "client.pem") + " --key " + shell_quote(dir + "key.pem") + " -o " +
         shell_quote(out_file) + " " + shell_quote(url) + " 2>/dev/null";
}

static void make_dirs(const std::string& path) {
  std::error_code ec;
  std::filesystem::create_directories(path, ec);
}

// `rm <dir>/*`: the files (and links) of the folder, not the folders
static void remove_files(const std::string& dir) {
  DIR* h = ::opendir(dir.c_str());
  if (!h) return;
  std::vector<std::string> names;
  while (const struct dirent* e = ::readdir(h)) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    struct stat st;
    if (::lstat((dir + "/" + n).c_str(), &st) == 0 && !S_ISDIR(st.st_mode)) names.push_back(n);
  }
  ::closedir(h);
  for (const std::string& n : names) ::unlink((dir + "/" + n).c_str());
}

static std::string curl_binary() {
  const char* zc = std::getenv("ZF_CURL");
  return (zc && *zc && ::access(zc, X_OK) == 0) ? zc : "curl";
}

UpdateResult update_games(Services& sv, const std::string& ip) {
  UpdateResult r;
  if (!valid_host(ip)) return r;
  const char* base = std::getenv("ZF_ML_TMP");
  std::string tmpl = std::string(base && *base ? base : "/tmp") + "/zf-ml-XXXXXX";
  std::vector<char> t(tmpl.begin(), tmpl.end());
  t.push_back('\0');
  if (!::mkdtemp(t.data())) return r;
  const std::string tmp = t.data(), list_file = tmp + "/applist.xml", curl = curl_binary();
  auto cleanup = [&] {
    ::unlink(list_file.c_str());
    ::rmdir(tmp.c_str());
  };
  CmdResult c = sv.cmd.run(request_command(curl, make_url(ip, "applist", std::string(), new_guid()), list_file), 30000);
  std::string xml;
  std::vector<App> apps;
  if (c.dry || c.exit_code != 0 || !read_file(list_file, xml) || !parse_applist(xml, apps)) {
    cleanup();
    return r;
  }
  const bool qt = qt_build();
  const std::string roms = paths().rooted(paths().roms), images = roms + "/images";
  std::string old_list;
  read_file(roms + "/gamelist.xml", old_list);  // before it goes with the old scripts
  remove_files(images);
  remove_files(roms);
  make_dirs(images);
  for (const App& a : apps) {
    const std::string f = script_name(a.title);
    write_atomic(roms + "/" + f + ".sh", script_text(ip, a.title, qt));
    bool digits = !a.id.empty() && a.id.size() < 20;
    for (char ch : a.id) digits = digits && ch >= '0' && ch <= '9';
    if (!digits) continue;  // the id goes into a request line
    sv.cmd.run(request_command(curl, make_url(ip, "appasset", "appid=" + a.id + "&AssetType=2&AssetIdx=0", new_guid()), images + "/" + f + ".png"), 30000);
  }
  write_atomic(roms + "/gamelist.xml", gamelist_text(apps, old_list));
  cleanup();
  r.ok = true;
  r.apps = apps.size();
  return r;
}

bool unpair() {
  std::error_code ec;
  bool any = false;
  auto rm = [&](const std::string& p) { any = std::filesystem::remove_all(p, ec) > 0 || any; };
  if (qt_build()) {
    rm(paths().rooted(paths().cache_qt));
    rm(paths().rooted(paths().config_qt_dir));
  } else {
    rm(paths().rooted(paths().cache_embedded));
  }
  return any;
}

void extract_certs() {
  if (!qt_build()) return;
  std::string conf;
  if (!read_file(paths().rooted(paths().conf_qt), conf)) return;
  const std::string dir = paths().rooted(paths().cache_qt);
  static const char* const kFiles[2][2] = {{"client.pem", "certificate"}, {"key.pem", "key"}};
  for (const auto& f : kFiles) {
    const std::string path = dir + "/" + f[0];
    if (file_exists(path)) continue;
    std::string pem = extract_pem(conf, f[1]);
    if (pem.empty()) continue;
    make_dirs(dir);
    write_atomic(path, pem);
  }
}

}  // namespace zf::moonlight
