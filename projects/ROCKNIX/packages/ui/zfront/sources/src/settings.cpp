#include "settings.hpp"

#include "services.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <pugixml.hpp>

#include "util.hpp"

namespace zf {

bool write_atomic(const std::string& path, const std::string& text, std::string* err) {
  if (writes_blocked()) return true;  // preview / dry-run: pretend (the in-memory state still follows)
  // a symlinked target (a config linked into /storage) is written through: the rename would replace the link itself
  std::string real = path;
  {
    struct stat ls;
    char rp[PATH_MAX];
    if (::lstat(path.c_str(), &ls) == 0 && S_ISLNK(ls.st_mode) && ::realpath(path.c_str(), rp)) real = rp;
  }
  // one temp name per write (pid + counter): two threads writing the same file never share a temp file
  static std::atomic<unsigned> counter{0};
  std::string tmp = real + ".zfront-tmp." + std::to_string(::getpid()) + "." + std::to_string(counter.fetch_add(1));
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    if (err) *err = tmp + ": cannot create";
    return false;
  }
  bool ok = true;
  struct stat st;
  if (::stat(real.c_str(), &st) == 0) (void)!::fchmod(fd, st.st_mode & 07777);  // keep the mode of the file it replaces
  for (size_t off = 0; ok && off < text.size();) {
    ssize_t w = ::write(fd, text.data() + off, text.size() - off);
    if (w <= 0) ok = false;
    else off += size_t(w);
  }
  ok = ok && ::fsync(fd) == 0;  // data on disk before the rename: a power cut must not leave an empty file
  ok = (::close(fd) == 0) && ok;
  if (!ok || std::rename(tmp.c_str(), real.c_str()) != 0) {
    if (err) *err = path + ": cannot write";
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

// ---- SystemConf

static bool split_kv(std::string_view line, std::string& k, std::string& v) {
  std::string_view t = trim(line);
  if (t.empty() || t[0] == '#' || t[0] == ';') return false;
  size_t eq = t.find('=');
  if (eq == std::string_view::npos) return false;
  k = std::string(trim(t.substr(0, eq)));
  v = std::string(trim(t.substr(eq + 1)));
  return true;
}

SystemConf SystemConf::from_text(const std::string& text, const std::string& path) {
  SystemConf c;
  c.path_ = path;
  size_t i = 0;
  while (i < text.size()) {
    size_t e = text.find('\n', i);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(i, e - i);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    c.lines_.push_back(line);
    i = e + 1;
  }
  std::string k, v;
  for (const auto& l : c.lines_)
    if (split_kv(l, k, v)) c.map_[k] = v;
  return c;
}

SystemConf SystemConf::load(const std::string& path) {
  std::string t;
  const bool ok = read_file(path, t);
  SystemConf c = from_text(ok ? t : std::string(), path);
  c.unreadable_ = !ok && ::access(path.c_str(), F_OK) == 0;  // there, but not readable: saving would wipe every setting
  return c;
}

const std::string* SystemConf::get(const std::string& key) const {
  auto it = map_.find(key);
  return it == map_.end() ? nullptr : &it->second;
}

std::string SystemConf::get_or(const std::string& key, const std::string& def) const {
  const std::string* v = get(key);
  return v ? *v : def;
}

bool SystemConf::get_bool(const std::string& key, bool def) const {
  const std::string* v = get(key);
  if (!v) return def;
  if (*v == "1" || *v == "true") return true;
  if (*v == "0" || *v == "false") return false;
  return def;
}

static bool line_has_key(const std::string& line, const std::string& key) {
  std::string_view t = trim(line);
  if (t.empty() || t[0] == '#' || t[0] == ';') return false;
  size_t eq = t.find('=');
  return eq != std::string_view::npos && trim(t.substr(0, eq)) == key;
}

// system.cfg is one `key=value` per line and shell scripts read it: a line break inside a key or value would write a
// second, forged entry ("x\nroot_injected=1"). Line breaks become spaces.
static std::string one_line(std::string s) {
  for (char& c : s)
    if (c == '\n' || c == '\r') c = ' ';
  return s;
}

void SystemConf::set(const std::string& key_in, const std::string& value_in) {
  const std::string key = one_line(key_in), value = one_line(value_in);
  map_[key] = value;
  std::string nl = key + "=" + value;
  bool found = false;
  for (auto& l : lines_)
    if (line_has_key(l, key)) {
      l = nl;
      found = true;
    }
  if (!found) lines_.push_back(nl);
}

void SystemConf::remove(const std::string& key_in) {
  const std::string key = one_line(key_in);
  map_.erase(key);
  std::vector<std::string> keep;
  for (auto& l : lines_)
    if (!line_has_key(l, key)) keep.push_back(l);
  lines_.swap(keep);
}

std::string SystemConf::text() const {
  std::string s;
  for (const auto& l : lines_) {
    s += l;
    s += '\n';
  }
  return s;
}

namespace {
std::atomic<unsigned> g_generation{0};
}
unsigned settings_generation() { return g_generation.load(std::memory_order_relaxed); }

bool SystemConf::save(std::string* err) const {
  if (unreadable_) {
    if (err) *err = "could not read system.cfg";
    return false;
  }
  bool ok = write_atomic(path_, text(), err);
  if (ok && !writes_blocked()) g_generation.fetch_add(1, std::memory_order_relaxed);
  return ok;
}

// ---- EsSettings

EsSettings EsSettings::from_text(const std::string& xml, const std::string& path) {
  EsSettings s;
  s.path_ = path;
  pugi::xml_document doc;
  const pugi::xml_parse_result parsed = doc.load_buffer(xml.data(), xml.size());
  // a non-empty file that does not parse is NOT an empty settings file: `save` refuses to write over it. A file that holds only the
  // declaration (ES writes that when every value is a default) has no element at all: that is empty, not corrupt.
  s.unreadable_ = !parsed && parsed.status != pugi::status_no_document_element && xml.find_first_not_of(" \t\r\n") != std::string::npos;
  if (parsed) {
    // ES writes the settings as top-level elements without a root; a wrapping element is accepted too.
    auto visit = [&](auto&& self, pugi::xml_node parent, int depth) -> void {
      for (pugi::xml_node n : parent.children()) {
        if (n.type() != pugi::node_element) continue;
        pugi::xml_attribute name = n.attribute("name"), val = n.attribute("value");
        if (name && val) s.map_[name.value()] = {n.name(), val.value()};
        else if (depth < 2) self(self, n, depth + 1);
      }
    };
    visit(visit, doc, 0);
  }
  return s;
}

EsSettings EsSettings::load(const std::string& path) {
  std::string t;
  read_file(path, t);
  return from_text(t, path);
}

const std::string* EsSettings::get(const std::string& name) const {
  auto it = map_.find(name);
  return it == map_.end() ? nullptr : &it->second.second;
}

std::vector<std::string> EsSettings::names() const {
  std::vector<std::string> v;
  v.reserve(map_.size());
  for (const auto& kv : map_) v.push_back(kv.first);
  return v;
}

bool EsSettings::get_bool(const std::string& name, bool def) const {
  const std::string* v = get(name);
  return v ? *v == "true" : def;
}

long EsSettings::get_int(const std::string& name, long def) const {
  const std::string* v = get(name);
  long r;
  return v && parse_int(*v, r) ? r : def;
}

void EsSettings::set_bool(const std::string& name, bool v) {
  map_[name] = {"bool", v ? "true" : "false"};
  touched_.insert(name);
}
void EsSettings::set_string(const std::string& name, const std::string& v) {
  map_[name] = {"string", v};
  touched_.insert(name);
}
void EsSettings::set_int(const std::string& name, long v) {
  map_[name] = {"int", std::to_string(v)};
  touched_.insert(name);
}

static std::string xml_esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default: o += c;
    }
  }
  return o;
}

static std::string es_text(const std::map<std::string, std::pair<std::string, std::string>>& m) {
  std::string s = "<?xml version=\"1.0\"?>\n<config>\n";  // same as the Rust skeleton; the reader accepts a root-less file too
  for (const auto& [k, tv] : m) s += "\t<" + tv.first + " name=\"" + xml_esc(k) + "\" value=\"" + xml_esc(tv.second) + "\" />\n";
  s += "</config>\n";
  return s;
}

std::string EsSettings::text() const { return es_text(map_); }

// ES and zfront take turns on this file: re-read it, keep what is on disk (unknown keys, values changed by ES or another tool
// meanwhile) and apply only the keys this object set since it was loaded / last saved.
bool EsSettings::save(std::string* err) const {
  if (writes_blocked()) return true;  // preview / dry-run: nothing is written, the in-memory state still follows
  std::string disk;
  const bool have = !path_.empty() && read_file(path_, disk);
  std::map<std::string, std::pair<std::string, std::string>> out;
  if (have && disk.find_first_not_of(" \t\r\n") != std::string::npos) {
    EsSettings fresh = EsSettings::from_text(disk, path_);
    if (fresh.unreadable_) {
      if (err) *err = path_ + ": unreadable, not overwritten";
      return false;
    }
    out = fresh.map_;
    for (const std::string& k : touched_) {
      auto it = map_.find(k);
      if (it != map_.end()) out[k] = it->second;
    }
  } else {
    out = map_;  // missing or empty file: this object's own settings are all there is
  }
  bool ok = write_atomic(path_, es_text(out), err);
  if (ok) {
    map_ = out;
    touched_.clear();
    g_generation.fetch_add(1, std::memory_order_relaxed);
  }
  return ok;
}

std::string& SystemConf::lock_path() {
  static std::string p = "/tmp/.system.cfg.lock";
  return p;
}

bool SystemConf::persist(const std::string& key, const std::string& value, std::string* err) {
  if (path_.empty()) {
    if (err) *err = "system.cfg path unknown";
    return false;
  }
  if (writes_blocked()) {  // dry-run: no lock, no disk; keep earlier in-memory changes
    set(key, value);
    return true;
  }
  ConfLock lk(lock_path());
  if (!lk.ok()) {
    if (err) *err = "system.cfg is locked";
    return false;
  }
  SystemConf fresh = SystemConf::load(path_);
  if (fresh.unreadable_) {
    if (err) *err = "could not read system.cfg";
    return false;
  }
  fresh.set(key, value);
  if (!fresh.save(err)) return false;
  *this = std::move(fresh);
  return true;
}

bool SystemConf::persist_remove(const std::string& key, std::string* err) {
  if (path_.empty()) {
    if (err) *err = "system.cfg path unknown";
    return false;
  }
  if (writes_blocked()) {
    remove(key);
    return true;
  }
  ConfLock lk(lock_path());
  if (!lk.ok()) {
    if (err) *err = "system.cfg is locked";
    return false;
  }
  SystemConf fresh = SystemConf::load(path_);
  if (fresh.unreadable_) {
    if (err) *err = "could not read system.cfg";
    return false;
  }
  fresh.remove(key);
  if (!fresh.save(err)) return false;
  *this = std::move(fresh);
  return true;
}

}  // namespace zf
