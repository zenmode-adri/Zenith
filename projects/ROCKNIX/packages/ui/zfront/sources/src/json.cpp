#include "json.hpp"

#include <cmath>
#include <cstdlib>

namespace zf::json {

namespace {

constexpr int kMaxDepth = 64;

struct P {
  std::string_view s;
  size_t i = 0;

  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }
  bool lit(const char* w) {
    size_t n = 0;
    while (w[n]) ++n;
    if (s.compare(i, n, w) != 0) return false;
    i += n;
    return true;
  }
  static void utf8(std::string& o, unsigned long cp) {
    if (cp < 0x80) {
      o.push_back(char(cp));
    } else if (cp < 0x800) {
      o.push_back(char(0xC0 | (cp >> 6)));
      o.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      o.push_back(char(0xE0 | (cp >> 12)));
      o.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      o.push_back(char(0x80 | (cp & 0x3F)));
    } else {
      o.push_back(char(0xF0 | (cp >> 18)));
      o.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      o.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      o.push_back(char(0x80 | (cp & 0x3F)));
    }
  }
  bool hex4(unsigned long& v) {
    if (i + 4 > s.size()) return false;
    v = 0;
    for (int k = 0; k < 4; ++k) {
      char c = s[i + size_t(k)];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
      else return false;
    }
    i += 4;
    return true;
  }
  bool str(std::string& o) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    while (i < s.size()) {
      unsigned char c = (unsigned char)s[i++];
      if (c == '"') return true;
      if (c < 0x20) return false;
      if (c != '\\') {
        o.push_back(char(c));
        continue;
      }
      if (i >= s.size()) return false;
      char e = s[i++];
      switch (e) {
        case '"': o.push_back('"'); break;
        case '\\': o.push_back('\\'); break;
        case '/': o.push_back('/'); break;
        case 'b': o.push_back('\b'); break;
        case 'f': o.push_back('\f'); break;
        case 'n': o.push_back('\n'); break;
        case 'r': o.push_back('\r'); break;
        case 't': o.push_back('\t'); break;
        case 'u': {
          unsigned long cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00) {  // high surrogate: needs the low one
            unsigned long lo;
            if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u') return false;
            i += 2;
            if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;
          }
          utf8(o, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool value(Value& v, int depth) {
    if (depth > kMaxDepth) return false;
    ws();
    if (i >= s.size()) return false;
    char c = s[i];
    if (c == '{') {
      ++i;
      v.type = Value::Type::Object;
      ws();
      if (i < s.size() && s[i] == '}') return ++i, true;
      for (;;) {
        ws();
        std::string k;
        if (!str(k)) return false;
        ws();
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        v.obj.emplace_back(std::move(k), Value());
        if (!value(v.obj.back().second, depth + 1)) return false;
        ws();
        if (i >= s.size()) return false;
        if (s[i] == ',') {
          ++i;
          continue;
        }
        if (s[i] == '}') return ++i, true;
        return false;
      }
    }
    if (c == '[') {
      ++i;
      v.type = Value::Type::Array;
      ws();
      if (i < s.size() && s[i] == ']') return ++i, true;
      for (;;) {
        v.arr.emplace_back();
        if (!value(v.arr.back(), depth + 1)) return false;
        ws();
        if (i >= s.size()) return false;
        if (s[i] == ',') {
          ++i;
          continue;
        }
        if (s[i] == ']') return ++i, true;
        return false;
      }
    }
    if (c == '"') {
      v.type = Value::Type::String;
      return str(v.str);
    }
    if (lit("true")) return v.type = Value::Type::Bool, v.b = true, true;
    if (lit("false")) return v.type = Value::Type::Bool, v.b = false, true;
    if (lit("null")) return v.type = Value::Type::Null, true;
    // number
    size_t b = i;
    if (i < s.size() && s[i] == '-') ++i;
    size_t digits = 0;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) ++i, ++digits;
    if (digits == 0 || i - b > 40) return false;
    std::string t(s.substr(b, i - b));
    char* end = nullptr;
    double d = std::strtod(t.c_str(), &end);
    if (!end || *end != '\0' || !std::isfinite(d)) return false;
    v.type = Value::Type::Number;
    v.num = d;
    return true;
  }
};

}  // namespace

const Value* Value::find(std::string_view key) const {
  if (type != Type::Object) return nullptr;
  for (const auto& kv : obj)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

std::string Value::text(std::string_view key) const {
  const Value* v = find(key);
  if (!v) return {};
  switch (v->type) {
    case Type::String: return v->str;
    case Type::Bool: return v->b ? "true" : "false";
    case Type::Number: {
      if (v->num == std::floor(v->num) && std::fabs(v->num) < 9e15) return std::to_string(static_cast<long long>(v->num));
      return std::to_string(v->num);
    }
    default: return {};
  }
}

long Value::integer(std::string_view key, long def) const {
  const Value* v = find(key);
  if (!v) return def;
  if (v->type == Type::Number) return static_cast<long>(v->num);
  if (v->type == Type::String) {
    char* end = nullptr;
    long r = std::strtol(v->str.c_str(), &end, 10);
    return end && end != v->str.c_str() && *end == '\0' ? r : def;
  }
  return def;
}

bool Value::boolean(std::string_view key) const {
  const Value* v = find(key);
  return v && v->type == Type::Bool && v->b;
}

bool parse(std::string_view text, Value& out) {
  out = Value();
  P p{text, 0};
  if (!p.value(out, 0)) {
    out = Value();
    return false;
  }
  p.ws();
  if (p.i != text.size()) {
    out = Value();
    return false;
  }
  return true;
}

}  // namespace zf::json
