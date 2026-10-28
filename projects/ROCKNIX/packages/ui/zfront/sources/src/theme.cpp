#include "theme.hpp"

#include <cmath>
#include <cstdio>
#include <pugixml.hpp>

#include "util.hpp"

namespace zf {

namespace {
struct Ratio {
  const char* name;
  float v;
};
const Ratio kRatios[] = {
    {"1/1", 1.0f},          {"4/3", 4.0f / 3.0f},   {"5/4", 5.0f / 4.0f},   {"3/2", 3.0f / 2.0f},   {"16/10", 16.0f / 10.0f}, {"16/9", 16.0f / 9.0f},
    {"19/12", 19.0f / 12.0f}, {"19/14", 19.0f / 14.0f}, {"30/17", 30.0f / 17.0f}, {"2/1", 2.0f},       {"21/9", 21.0f / 9.0f},
};

// Visit `<variables>` in document order (any depth), like XML `descendants()`.
// Bounded depth: a theme nested 50000 levels would exhaust the stack (real ones nest 3-4 deep).
template <class F>
void each_variables(pugi::xml_node n, F&& f, int depth = 0) {
  if (depth >= 64) return;
  for (pugi::xml_node c : n.children()) {
    if (c.type() != pugi::node_element) continue;
    if (std::string_view(c.name()) == "variables") f(c);
    each_variables(c, f, depth + 1);
  }
}

bool read_variables(const std::string& xml, const std::string& ratio, std::unordered_map<std::string, std::string>& out, std::string& err) {
  pugi::xml_document doc;
  pugi::xml_parse_result r = doc.load_buffer(xml.data(), xml.size());
  if (!r) {
    err = r.description();
    return false;
  }
  std::string want = "'" + ratio + "'";
  // unconditional blocks first, then the ones whose `if` matches, so they override
  for (int pass = 0; pass < 2; ++pass) {
    each_variables(doc, [&](pugi::xml_node v) {
      pugi::xml_attribute cond = v.attribute("if");
      bool ok = false;
      if (pass == 0) ok = !cond;
      else if (cond) {
        std::string_view c = cond.value();
        ok = c.find("screen.ratio") != std::string_view::npos && c.find(want) != std::string_view::npos;
      }
      if (!ok) return;
      for (pugi::xml_node e : v.children()) {
        if (e.type() != pugi::node_element) continue;
        pugi::xml_node t = e.first_child();
        std::string text = (t && (t.type() == pugi::node_pcdata || t.type() == pugi::node_cdata)) ? t.value() : "";
        out[e.name()] = std::string(trim(text));
      }
    });
  }
  return true;
}
}  // namespace

const char* ratio_name(unsigned w, unsigned h) {
  float r = float(w) / float(h);
  const char* best = "1/1";
  float bd = 1e30f;
  for (const auto& x : kRatios) {
    float d = std::fabs(x.v - r);
    if (d < bd) {  // first minimum wins, as min_by does
      bd = d;
      best = x.name;
    }
  }
  return best;
}

bool Theme::from_xml(const std::string& layout_xml, const std::string& theme_xml, const std::string& ratio, Theme& out, std::string& err) {
  out.vars_.clear();
  if (!read_variables(layout_xml, ratio, out.vars_, err)) return false;
  return read_variables(theme_xml, ratio, out.vars_, err);
}

bool Theme::load(const std::string& dir, const std::string& ratio, Theme& out, std::string& err) {
  std::string theme_xml, layout_xml;
  if (!read_file(path_join(dir, "theme.xml"), theme_xml)) {
    err = "theme.xml: No such file or directory";
    return false;
  }
  if (!read_file(path_join(dir, "layout.xml"), layout_xml)) {
    err = "layout.xml: No such file or directory";
    return false;
  }
  out.dir_ = dir;
  return from_xml(layout_xml, theme_xml, ratio, out, err);
}

std::string_view Theme::str(std::string_view name) const {
  auto it = vars_.find(std::string(name));
  return it == vars_.end() ? std::string_view() : std::string_view(it->second);
}

bool Theme::has(std::string_view name) const { return vars_.count(std::string(name)) != 0; }

float Theme::f(std::string_view name) const {
  float v = 0;
  if (!parse_float(str(name), v)) {
    std::fprintf(stderr, "theme: variable '%.*s' missing or not a number\n", int(name.size()), name.data());
    return 0.0f;
  }
  return v;
}

Color Theme::color(std::string_view name) const { return parse_color(str(name)); }

std::string Theme::path(std::string_view rel) const {
  while (starts_with(rel, "./")) rel.remove_prefix(2);
  return path_join(dir_, rel);
}

Color parse_color(std::string_view s) {
  auto h = [&](size_t i) -> uint8_t {
    if (i + 2 > s.size()) return 0;
    unsigned v = 0;
    for (size_t k = i; k < i + 2; ++k) {
      char c = s[k];
      unsigned d;
      if (c >= '0' && c <= '9') d = unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') d = unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') d = unsigned(c - 'A' + 10);
      else return 0;
      v = v * 16 + d;
    }
    return uint8_t(v);
  };
  return {h(0), h(2), h(4), s.size() >= 8 ? h(6) : uint8_t(255)};
}

}  // namespace zf
