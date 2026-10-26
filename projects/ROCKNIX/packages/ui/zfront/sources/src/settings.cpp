#include "settings.hpp"

#include "services.hpp"

#include <atomic>
#include <cstdio>
#include <pugixml.hpp>

#include "util.hpp"

namespace zf {

bool write_atomic(const std::string& path, const std::string& text, std::string* err) {
  if (writes_blocked()) return true;  // preview / dry-run: pretend (the in-memory state still follows)
  std::string tmp = path + ".zfront-tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) {
    if (err) *err = tmp + ": cannot create";
    return false;
  }
  bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  ok = (std::fflush(f) == 0) && ok;
  ok = (std::fclose(f) == 0) && ok;
  if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
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
  read_file(path, t);
  return from_text(t, path);
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

void SystemConf::set(const std::string& key, const std::string& value) {
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

void SystemConf::remove(const std::string& key) {
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
  bool ok = write_atomic(path_, text(), err);
  if (ok && !writes_blocked()) g_generation.fetch_add(1, std::memory_order_relaxed);
  return ok;
}

// ---- EsSettings

EsSettings EsSettings::from_text(const std::string& xml, const std::string& path) {
  EsSettings s;
  s.path_ = path;
  pugi::xml_document doc;
  if (doc.load_buffer(xml.data(), xml.size())) {
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

bool EsSettings::get_bool(const std::string& name, bool def) const {
  const std::string* v = get(name);
  return v ? *v == "true" : def;
}

long EsSettings::get_int(const std::string& name, long def) const {
  const std::string* v = get(name);
  long r;
  return v && parse_int(*v, r) ? r : def;
}

void EsSettings::set_bool(const std::string& name, bool v) { map_[name] = {"bool", v ? "true" : "false"}; }
void EsSettings::set_string(const std::string& name, const std::string& v) { map_[name] = {"string", v}; }
void EsSettings::set_int(const std::string& name, long v) { map_[name] = {"int", std::to_string(v)}; }

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

std::string EsSettings::text() const {
  std::string s = "<?xml version=\"1.0\"?>\n<config>\n";  // same as the Rust skeleton; the reader accepts a root-less file too
  for (const auto& [k, tv] : map_) s += "\t<" + tv.first + " name=\"" + xml_esc(k) + "\" value=\"" + xml_esc(tv.second) + "\" />\n";
  s += "</config>\n";
  return s;
}

bool EsSettings::save(std::string* err) const {
  bool ok = write_atomic(path_, text(), err);
  if (ok && !writes_blocked()) g_generation.fetch_add(1, std::memory_order_relaxed);
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
  fresh.remove(key);
  if (!fresh.save(err)) return false;
  *this = std::move(fresh);
  return true;
}

}  // namespace zf
