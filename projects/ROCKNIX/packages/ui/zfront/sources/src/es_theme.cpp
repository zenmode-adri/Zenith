// ES theme reader: includes, subsets, variables, `if` conditions and `${}` (see es_theme.hpp). The mapping to zfront is in es_theme_map.cpp.
#include "es_theme.hpp"

#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <pugixml.hpp>

#include "util.hpp"

namespace zf::estheme {

namespace {

constexpr int kMaxFeatureNest = 16;   // nested <feature> (a corrupt theme must not overflow the stack)
constexpr int kMaxDepth = 8;          // nested <include>
constexpr int kMaxIncludes = 96;      // files read in total
constexpr size_t kMaxXml = 2u << 20;  // bytes per XML file
constexpr size_t kMaxElems = 20000;
constexpr int kMaxNest = 24;          // element nesting
constexpr size_t kMaxValue = 64u << 10;  // bytes of one value after ${} (variables that repeat each other would grow without end)

// ---- expressions -----------------------------------------------------------------------------------------------------------

struct Val {
  bool num = false;
  double n = 0;
  std::string s;
};

Val make_num(double n) {
  Val v;
  v.num = true;
  v.n = n;
  return v;
}
Val make_str(std::string s) {
  Val v;
  v.s = std::move(s);
  return v;
}
// A string that is a whole number counts as a number (`'720' == 720`).
bool as_number(const Val& v, double& out) {
  if (v.num) {
    out = v.n;
    return true;
  }
  float f = 0;
  if (!v.s.empty() && parse_float(v.s, f)) {
    out = f;
    return true;
  }
  return false;
}
bool truthy(const Val& v) {
  if (v.num) return v.n != 0.0;
  double n;
  if (as_number(v, n)) return n != 0.0;
  return !v.s.empty() && v.s != "false";
}

std::string num_text(double n);

class Expr {
 public:
  Expr(const std::string& text, const std::map<std::string, std::string>& vars) : t_(text), vars_(vars) {}
  bool run(Val& out) {
    if (t_.size() > 1024) return false;
    out = ternary();
    skip();
    return ok_ && pos_ == t_.size();
  }

 private:
  const std::string& t_;
  const std::map<std::string, std::string>& vars_;
  size_t pos_ = 0;
  bool ok_ = true;
  int depth_ = 0;

  void skip() {
    while (pos_ < t_.size() && (t_[pos_] == ' ' || t_[pos_] == '\t' || t_[pos_] == '\n' || t_[pos_] == '\r')) ++pos_;
  }
  bool eat(const char* op) {
    skip();
    size_t n = std::strlen(op);
    if (t_.compare(pos_, n, op) == 0) {
      pos_ += n;
      return true;
    }
    return false;
  }
  bool peek2(const char* op) {
    skip();
    return t_.compare(pos_, std::strlen(op), op) == 0;
  }
  std::string lookup(const std::string& k) const {
    auto it = vars_.find(k);
    return it == vars_.end() ? std::string() : it->second;
  }

  Val ternary() {
    Val c = orx();
    if (ok_ && eat("?")) {
      Val a = ternary();
      if (!eat(":")) {
        ok_ = false;
        return {};
      }
      Val b = ternary();
      return truthy(c) ? a : b;
    }
    return c;
  }
  Val orx() {
    Val l = andx();
    while (ok_ && eat("||")) {
      Val r = andx();
      l = make_num((truthy(l) || truthy(r)) ? 1 : 0);
    }
    return l;
  }
  Val andx() {
    Val l = cmp();
    while (ok_ && eat("&&")) {
      Val r = cmp();
      l = make_num((truthy(l) && truthy(r)) ? 1 : 0);
    }
    return l;
  }
  Val cmp() {
    Val l = add();
    for (;;) {
      if (!ok_) return l;
      int op = 0;  // 1 ==, 2 !=, 3 <=, 4 >=, 5 <, 6 >
      if (eat("==")) op = 1;
      else if (eat("!=")) op = 2;
      else if (eat("<=")) op = 3;
      else if (eat(">=")) op = 4;
      else if (peek2("<") ) { eat("<"); op = 5; }
      else if (peek2(">")) { eat(">"); op = 6; }
      else return l;
      Val r = add();
      double a, b;
      bool res;
      if (as_number(l, a) && as_number(r, b)) {
        res = op == 1 ? a == b : op == 2 ? a != b : op == 3 ? a <= b : op == 4 ? a >= b : op == 5 ? a < b : a > b;
      } else {
        int c = (l.num ? num_text(l.n) : l.s).compare(r.num ? num_text(r.n) : r.s);
        res = op == 1 ? c == 0 : op == 2 ? c != 0 : op == 3 ? c <= 0 : op == 4 ? c >= 0 : op == 5 ? c < 0 : c > 0;
      }
      l = make_num(res ? 1 : 0);
    }
  }
  Val add() {
    Val l = mul();
    for (;;) {
      if (!ok_) return l;
      skip();
      if (pos_ < t_.size() && (t_[pos_] == '+' || t_[pos_] == '-')) {
        char op = t_[pos_++];
        Val r = mul();
        double a = 0, b = 0;
        as_number(l, a);
        as_number(r, b);
        l = make_num(op == '+' ? a + b : a - b);
      } else {
        return l;
      }
    }
  }
  Val mul() {
    Val l = unary();
    for (;;) {
      if (!ok_) return l;
      skip();
      if (pos_ < t_.size() && (t_[pos_] == '*' || t_[pos_] == '/')) {
        char op = t_[pos_++];
        Val r = unary();
        double a = 0, b = 0;
        as_number(l, a);
        as_number(r, b);
        l = make_num(op == '*' ? a * b : (b == 0.0 ? 0.0 : a / b));
      } else {
        return l;
      }
    }
  }
  Val unary() {
    skip();
    if (pos_ < t_.size() && t_[pos_] == '!' && !(pos_ + 1 < t_.size() && t_[pos_ + 1] == '=')) {
      ++pos_;
      return make_num(truthy(unary()) ? 0 : 1);
    }
    if (pos_ < t_.size() && t_[pos_] == '-') {
      ++pos_;
      double a = 0;
      Val v = unary();
      as_number(v, a);
      return make_num(-a);
    }
    return primary();
  }
  Val primary() {
    skip();
    if (++depth_ > 32 || pos_ >= t_.size()) {
      ok_ = false;
      return {};
    }
    struct Dec {
      int& d;
      ~Dec() { --d; }
    } dec{depth_};
    char c = t_[pos_];
    if (c == '(') {
      ++pos_;
      Val v = ternary();
      if (!eat(")")) ok_ = false;
      return v;
    }
    if (c == '\'' || c == '"') {
      size_t e = t_.find(c, pos_ + 1);
      if (e == std::string::npos) {
        ok_ = false;
        return {};
      }
      std::string s = t_.substr(pos_ + 1, e - pos_ - 1);
      pos_ = e + 1;
      return make_str(s);
    }
    if (c == '{') {  // {screen.ratio}: a variable, a string
      size_t e = t_.find('}', pos_);
      if (e == std::string::npos) {
        ok_ = false;
        return {};
      }
      std::string k = t_.substr(pos_ + 1, e - pos_ - 1);
      pos_ = e + 1;
      return make_str(lookup(k));
    }
    if ((c >= '0' && c <= '9') || c == '.') {
      size_t s = pos_;
      while (pos_ < t_.size() && ((t_[pos_] >= '0' && t_[pos_] <= '9') || t_[pos_] == '.')) ++pos_;
      float f = 0;
      if (!parse_float(std::string_view(t_).substr(s, pos_ - s), f)) ok_ = false;
      return make_num(f);
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {  // bare name: a variable (or true/false)
      size_t s = pos_;
      while (pos_ < t_.size() && (std::isalnum(static_cast<unsigned char>(t_[pos_])) || t_[pos_] == '_' || t_[pos_] == '.' || t_[pos_] == '-')) ++pos_;
      std::string k = t_.substr(s, pos_ - s);
      if (k == "true") return make_num(1);
      if (k == "false") return make_num(0);
      auto it = vars_.find(k);
      return make_str(it == vars_.end() ? std::string() : it->second);
    }
    ok_ = false;
    return {};
  }
};

std::string num_text(double n) {
  char b[64];
  std::snprintf(b, sizeof b, "%.10g", n);
  return b;
}

// ---- paths -------------------------------------------------------------------------------------------------------------------

bool read_small(const std::string& path, std::string& out) {
  struct stat sb;
  if (::stat(path.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size < 0 || size_t(sb.st_size) > kMaxXml) return false;
  return read_file(path, out);
}

/// Lexical `base_dir/rel` inside `root` (both without a trailing slash). Empty when `rel` is absolute, `~`, or leaves the root.
std::string inside(const std::string& root, const std::string& base_dir, const std::string& rel) {
  if (rel.empty() || rel[0] == '/' || rel[0] == '~' || rel.find('\\') != std::string::npos || rel.find('\0') != std::string::npos) return {};
  std::string rest = base_dir.size() > root.size() ? base_dir.substr(root.size() + 1) : std::string();  // base relative to root
  std::vector<std::string> parts;
  auto push = [&](const std::string& p) -> bool {
    size_t i = 0;
    while (i <= p.size()) {
      size_t j = p.find('/', i);
      if (j == std::string::npos) j = p.size();
      std::string c = p.substr(i, j - i);
      if (c == "..") {
        if (parts.empty()) return false;
        parts.pop_back();
      } else if (!c.empty() && c != ".") {
        parts.push_back(c);
      }
      i = j + 1;
    }
    return true;
  };
  if (!push(rest) || !push(rel)) return {};
  std::string out = root;
  for (const std::string& p : parts) out += "/" + p;
  return out;
}

/// True when the real path of `path` is under the real path of `root` (a symlink in the theme must not lead out of it).
bool real_inside(const std::string& root_real, const std::string& path) {
  char buf[PATH_MAX];
  if (!::realpath(path.c_str(), buf)) return false;
  std::string p = buf;
  return p.size() > root_real.size() && p.compare(0, root_real.size(), root_real) == 0 && p[root_real.size()] == '/';
}

// ---- the resolver ------------------------------------------------------------------------------------------------------------

struct Ctx {
  const Options& opt;
  Resolved& out;
  std::map<std::string, std::string> ev;  // evaluator variables: screen.*, and every <variables> entry
  std::string root_real;
  int files = 0;
  bool truncated = false;
  std::map<std::string, size_t> index;  // "view\ttype\tname\tparent" -> out.elems index

  void warn(const std::string& w) {
    if (out.warnings.size() < 40) out.warnings.push_back(w);
  }
};

std::string subst(Ctx& c, const std::string& s) {
  if (s.find("${") == std::string::npos) return s;
  std::string o;
  o.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{') {
      size_t e = s.find('}', i + 2);
      if (e != std::string::npos) {
        std::string k = s.substr(i + 2, e - i - 2);
        auto it = c.out.vars.find(k);
        if (it != c.out.vars.end()) {
          o += it->second;
        } else {
          auto e2 = c.ev.find(k);
          o += e2 != c.ev.end() ? e2->second : s.substr(i, e - i + 1);  // unknown (${system.theme}): left as is
        }
        i = e + 1;
        continue;
      }
    }
    o += s[i++];
    if (o.size() > kMaxValue) break;
  }
  if (o.size() > kMaxValue) {
    c.truncated = true;
    o.resize(kMaxValue);
  }
  return o;
}

bool one_of(const std::string& list, const std::string& v, const char* seps) {
  size_t i = 0;
  while (i <= list.size()) {
    size_t j = list.find_first_of(seps, i);
    if (j == std::string::npos) j = list.size();
    if (std::string(trim(std::string_view(list).substr(i, j - i))) == v) return true;
    i = j + 1;
  }
  return false;
}

/// Selected option of a subset: the ES setting if it names an existing option, else the theme's default.
std::string subset_value(Ctx& c, const std::string& name) {
  auto chosen = c.out.subsets.find(name);
  if (chosen != c.out.subsets.end()) return chosen->second;
  return {};
}

bool pass_filters(Ctx& c, const pugi::xml_node& n) {
  if (pugi::xml_attribute a = n.attribute("if")) {
    std::string e = subst(c, a.value());
    bool ok = false;
    if (!eval_condition(e, c.ev, ok) || !ok) return false;
  }
  if (pugi::xml_attribute a = n.attribute("lang")) {
    if (!one_of(a.value(), c.opt.lang, ", ")) return false;
  }
  if (pugi::xml_attribute a = n.attribute("region")) {
    const std::string* r = c.opt.setting ? c.opt.setting("ThemeRegionName") : nullptr;
    if (!r || !one_of(a.value(), *r, ", ")) return false;
  }
  if (pugi::xml_attribute a = n.attribute("tinyScreen")) {
    bool small = c.opt.width <= 480 || c.opt.height <= 480;
    if (small != (std::string(a.value()) == "true")) return false;
  }
  if (pugi::xml_attribute a = n.attribute("verticalScreen")) {
    bool vert = c.opt.height > c.opt.width;
    if (vert != (std::string(a.value()) == "true")) return false;
  }
  if (pugi::xml_attribute a = n.attribute("ifCheevos")) {
    if (std::string(a.value()) == "true") return false;
  }
  if (pugi::xml_attribute a = n.attribute("ifSubset")) {
    {
      size_t i = 0;
      std::string all(a.value());
      while (i <= all.size()) {
        size_t j = all.find(',', i);
        if (j == std::string::npos) j = all.size();
        std::string item(trim(std::string_view(all).substr(i, j - i)));
        i = j + 1;
        size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        std::string sname(trim(std::string_view(item).substr(0, colon))), sval(trim(std::string_view(item).substr(colon + 1)));
        std::string sel = subset_value(c, sname);
        if (sel.empty()) {
          // a subset we know nothing about (no <subset>, no setting): ES lets the element through
          const std::string* st = c.opt.setting ? c.opt.setting("subset." + sname) : nullptr;
          if (st && !st->empty()) sel = *st;
        }
        if (sel.empty()) continue;
        if (!one_of(sval, sel, "|")) return false;
      }
    }
  }
  return true;
}

// Subsets: a `<subset name=X>` block lists the options as <include name=...>; the first that passes its filters is the default;
// the ES setting overrides it when it names one of the options.
void pick_subsets(Ctx& c, const pugi::xml_node& root) {
  for (pugi::xml_node s : root.children("subset")) {
    std::string name = s.attribute("name").as_string();
    if (name.empty() || !pass_filters(c, s)) continue;
    if (c.out.subsets.count(name)) continue;  // an earlier file already chose it
    std::vector<std::string> opts;
    for (pugi::xml_node inc : s.children("include"))
      if (pass_filters(c, inc) && inc.attribute("name")) opts.push_back(inc.attribute("name").as_string());
    if (opts.empty()) continue;
    std::string pick = opts.front();
    const std::string* st = nullptr;
    if (c.opt.setting) {
      if (name == "colorset") st = c.opt.setting("ThemeColorSet");
      else if (name == "iconset") st = c.opt.setting("ThemeIconSet");
      else if (name == "menu") st = c.opt.setting("ThemeMenu");
      else if (name == "systemview") st = c.opt.setting("ThemeSystemView");
      else if (name == "gamelistview") st = c.opt.setting("ThemeGamelistView");
      else if (name == "region") st = c.opt.setting("ThemeRegionName");
      if (!st || st->empty()) st = c.opt.setting("subset." + name);
    }
    if (st && std::find(opts.begin(), opts.end(), *st) != opts.end()) pick = *st;
    c.out.subsets[name] = pick;
    c.ev["subset." + name] = pick;
  }
}

void set_var(Ctx& c, const std::string& key, const std::string& raw) {
  std::string val = raw;
  if (val == "true" || val == "false") {
    c.out.vars[key] = val;
    c.ev[key] = val == "true" ? "1" : "0";
    return;
  }
  val = subst(c, val);
  if (raw.find("${") != std::string::npos || raw.find('=') != std::string::npos || raw.find('>') != std::string::npos || raw.find('<') != std::string::npos) {
    Val v;
    Expr ex(val, c.ev);
    // only expressions that are really arithmetic/comparisons: a path with a `=` in it stays text
    bool exprlike = val.find_first_of("+*/()<>=!&|?") != std::string::npos && val.find_first_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") == std::string::npos;
    if (exprlike && ex.run(v)) val = v.num ? num_text(v.n) : v.s;
  }
  c.out.vars[key] = val;
  c.ev[key] = val;
}

void parse_variables(Ctx& c, const pugi::xml_node& root) {
  for (pugi::xml_node vs : root.children("variables")) {
    if (!pass_filters(c, vs)) continue;
    for (pugi::xml_node v : vs.children()) {
      if (v.type() != pugi::node_element || !pass_filters(c, v)) continue;
      std::string key = v.name();
      std::string val = v.text().as_string();
      if (key == "variable" && v.attribute("name") && v.attribute("value")) {
        key = v.attribute("name").as_string();
        val = v.attribute("value").as_string();
      }
      if (key.empty() || key.size() > 128) continue;
      set_var(c, key, std::string(trim(val)));
    }
  }
}

bool has_element_child(const pugi::xml_node& n) {
  for (pugi::xml_node k : n.children())
    if (k.type() == pugi::node_element) return true;
  return false;
}

Element& element_for(Ctx& c, const std::string& view, const std::string& type, const std::string& name, const std::string& parent) {
  std::string key = view + "\t" + type + "\t" + name + "\t" + parent;
  auto it = c.index.find(key);
  if (it != c.index.end()) return c.out.elems[it->second];
  c.index[key] = c.out.elems.size();
  c.out.elems.push_back({view, type, name, {}, parent});
  return c.out.elems.back();
}

std::vector<std::string> split_names(const std::string& s) {
  std::vector<std::string> v;
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find_first_of(" \t\r\n,", i);
    if (j == std::string::npos) j = s.size();
    if (j > i) v.push_back(s.substr(i, j - i));
    i = j + 1;
  }
  return v;
}

void parse_element(Ctx& c, const pugi::xml_node& n, const std::vector<std::string>& views, const std::string& parent, int nest) {
  if (nest > kMaxNest || c.out.elems.size() >= kMaxElems) {
    c.truncated = true;
    return;
  }
  if (!pass_filters(c, n)) return;
  std::string type = n.name();
  if (type == "storyboard" || type == "variables" || type == "animation") return;
  std::vector<std::string> names = split_names(n.attribute("name").as_string());
  if (names.empty()) names.push_back("");
  for (const std::string& view : views) {
    for (const std::string& name : names) {
      if (c.out.elems.size() >= kMaxElems) {
        c.truncated = true;
        return;
      }
      Element& e = element_for(c, view, type, name, parent);
      for (pugi::xml_node p : n.children()) {
        if (p.type() != pugi::node_element) continue;
        if (has_element_child(p) || std::string_view(p.name()) == "container") continue;  // nested element: below
        if (!pass_filters(c, p)) continue;
        std::string key = p.name();
        if (key == "storyboard" || key == "animation") continue;
        e.props[key] = subst(c, std::string(trim(p.text().as_string())));
      }
    }
  }
  for (pugi::xml_node p : n.children()) {
    if (p.type() != pugi::node_element) continue;
    std::string_view pn = p.name();
    if (pn == "storyboard") continue;
    if (has_element_child(p) || pn == "container") {
      std::string childtype = type;
      parse_element(c, p, views, childtype, nest + 1);
    }
  }
}

void process_file(Ctx& c, const std::string& file, int depth, bool is_root);

// What THEME CONFIGURATION needs of a `<subset>` (ES parseSubset): its name, displayName and appliesTo, and the options that pass their filters.
// The texts stay raw until the whole theme is read (`finish_subsets`): they may name variables of a file read later.
void collect_subset(Ctx& c, const pugi::xml_node& n) {
  const std::string name = n.attribute("name").as_string();
  if (name.empty() || c.out.subset_infos.size() >= 64) return;
  SubsetInfo* info = nullptr;
  for (SubsetInfo& i : c.out.subset_infos)
    if (i.name == name) info = &i;
  if (!info) {
    c.out.subset_infos.emplace_back();
    info = &c.out.subset_infos.back();
    info->name = name;
  }
  if (info->display.empty()) info->display = n.attribute("displayName").as_string();
  if (info->applies_to.empty()) {
    const std::string all = n.attribute("appliesTo").as_string();
    size_t i = 0;
    while (i <= all.size()) {
      size_t j = all.find_first_of(", ", i);
      if (j == std::string::npos) j = all.size();
      std::string item(trim(std::string_view(all).substr(i, j - i)));
      if (!item.empty()) info->applies_to.push_back(item);
      i = j + 1;
    }
  }
  for (pugi::xml_node inc : n.children("include")) {
    const std::string oname = inc.attribute("name").as_string();
    if (oname.empty() || !pass_filters(c, inc) || info->options.size() >= 256) continue;
    bool have = false;
    for (const SubsetOption& o : info->options) have = have || o.name == oname;
    if (!have) info->options.push_back({oname, inc.attribute("displayName").as_string()});
  }
}

// ${} in the names shown, now that every variable is known; ES falls back to the option's name, and for a subset to the variable `subset.<name>`
void finish_subsets(Ctx& c) {
  for (SubsetInfo& i : c.out.subset_infos) {
    if (i.display.empty()) {
      auto v = c.out.vars.find("subset." + i.name);
      if (v != c.out.vars.end()) i.display = v->second;
    }
    i.display = subst(c, i.display);
    for (SubsetOption& o : i.options) o.display = o.display.empty() ? o.name : subst(c, o.display);
  }
  c.out.subset_infos.erase(std::remove_if(c.out.subset_infos.begin(), c.out.subset_infos.end(), [](const SubsetInfo& i) { return i.options.empty(); }),
                           c.out.subset_infos.end());
}

void parse_children(Ctx& c, const pugi::xml_node& root, const std::string& file, int depth, int nest = 0) {
  const std::string dir = path_dir(file);
  for (pugi::xml_node n : root.children()) {
    if (n.type() != pugi::node_element) continue;
    std::string tag = n.name();
    if (tag == "include") {
      if (!pass_filters(c, n)) continue;
      std::string rel = subst(c, std::string(trim(n.text().as_string())));
      if (rel.empty()) continue;  // a subset option with no file
      std::string path = inside(c.out.root, dir, rel);
      if (path.empty()) {
        c.warn("include ignored (outside the theme or user path): " + rel);
        continue;
      }
      if (rel.find("${") != std::string::npos) {
        // an include named by a variable we could not resolve ({system.theme}, ${lang}...)
        c.warn("include ignored (unresolved variable): " + rel);
        continue;
      }
      if (!file_exists(path)) {
        c.warn("include not found: " + rel);
        continue;
      }
      if (!real_inside(c.root_real, path)) {
        c.warn("include ignored (leaves the theme): " + rel);
        continue;
      }
      if (depth + 1 > kMaxDepth || c.files >= kMaxIncludes) {
        c.warn("include ignored (too deep or too many): " + rel);
        continue;
      }
      process_file(c, path, depth + 1, false);
    } else if (tag == "view" || tag == "customView") {
      if (!pass_filters(c, n)) continue;
      std::vector<std::string> views = split_names(n.attribute("name").as_string());
      if (views.empty()) continue;
      for (pugi::xml_node e : n.children())
        if (e.type() == pugi::node_element) parse_element(c, e, views, "", 0);
    } else if (tag == "feature") {
      if (!pass_filters(c, n)) continue;
      if (nest + 1 > kMaxFeatureNest) {
        c.warn("feature ignored (nested too deep)");
        continue;
      }
      parse_children(c, n, file, depth, nest + 1);
    } else if (tag == "subset") {
      if (!pass_filters(c, n)) continue;
      // options that carry a file: <include name="x">./x.xml</include> loads only when selected
      std::string sname = n.attribute("name").as_string();
      collect_subset(c, n);
      std::string sel = subset_value(c, sname);
      for (pugi::xml_node inc : n.children("include")) {
        if (!pass_filters(c, inc) || std::string(inc.attribute("name").as_string()) != sel) continue;
        std::string rel = subst(c, std::string(trim(inc.text().as_string())));
        if (rel.empty()) continue;
        std::string path = inside(c.out.root, dir, rel);
        if (path.empty() || !file_exists(path) || !real_inside(c.root_real, path) || depth + 1 > kMaxDepth || c.files >= kMaxIncludes) {
          c.warn("subset file ignored: " + rel);
          continue;
        }
        process_file(c, path, depth + 1, false);
      }
    }
  }
}

void process_file(Ctx& c, const std::string& file, int depth, bool is_root) {
  std::string xml;
  if (!read_small(file, xml)) {
    c.warn("cannot read " + file);
    return;
  }
  ++c.files;
  pugi::xml_document doc;
  pugi::xml_parse_result r = doc.load_buffer(xml.data(), xml.size());
  if (!r) {
    c.warn(path_filename(file) + ": " + r.description());
    return;
  }
  pugi::xml_node root = doc.child("theme");
  if (!root) {
    c.warn(path_filename(file) + ": no <theme>");
    return;
  }
  pick_subsets(c, root);
  parse_variables(c, root);
  parse_children(c, root, file, depth);
}

}  // namespace

std::string theme_file(const std::string& root_in, const std::string& rel) {
  std::string root = root_in;
  while (root.size() > 1 && root.back() == '/') root.pop_back();
  std::string p = inside(root, root, rel);
  if (p.empty() || !file_exists(p)) return {};
  char buf[PATH_MAX];
  if (!::realpath(root.c_str(), buf)) return {};
  return real_inside(buf, p) ? p : std::string();
}

bool eval_condition(const std::string& expr, const std::map<std::string, std::string>& vars, bool& ok) {
  Val v;
  Expr e(expr, vars);
  ok = false;
  if (!e.run(v)) return false;
  ok = truthy(v);
  return true;
}

bool resolve(const std::string& dir_in, const Options& opt, Resolved& out, std::string& err) {
  out = Resolved();
  std::string dir = dir_in;
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  out.root = dir;
  const std::string main_xml = dir + "/theme.xml";
  if (!file_exists(main_xml)) {
    err = "theme.xml: No such file or directory";
    return false;
  }
  char buf[PATH_MAX];
  if (!::realpath(dir.c_str(), buf)) {
    err = "cannot resolve " + dir;
    return false;
  }
  Ctx c{opt, out, {}, buf, 0, false, {}};
  c.ev["screen.width"] = std::to_string(opt.width);
  c.ev["screen.height"] = std::to_string(opt.height);
  c.ev["screen.ratio"] = opt.ratio;
  c.ev["screen.vertical"] = opt.height > opt.width ? "1" : "0";
  c.ev["global.language"] = opt.lang;
  c.ev["lang"] = opt.lang;
  out.vars["lang"] = opt.lang;
  process_file(c, main_xml, 0, true);
  if (c.files == 0) {
    err = "theme.xml cannot be read";
    return false;
  }
  if (out.elems.empty() && out.vars.size() <= 1) {
    err = "theme.xml has no views or variables";
    return false;
  }
  if (c.truncated) out.warnings.push_back("theme truncated (too many elements or nesting)");
  finish_subsets(c);
  return true;
}

std::vector<SubsetInfo> list_subsets(const std::string& dir, const Options& opt) {
  Resolved r;
  std::string err;
  if (!resolve(dir, opt, r, err)) return {};
  return std::move(r.subset_infos);
}

}  // namespace zf::estheme
