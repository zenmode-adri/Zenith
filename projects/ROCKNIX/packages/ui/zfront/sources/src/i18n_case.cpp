// Sentence case for the UI labels that ES writes in capitals ("GAME SETTINGS" -> "Game settings"). Used as the fallback
// of tr() when no catalog translates a key (see `display_text`): a translation found in a catalog is shown as it is.
#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>

#include "i18n.hpp"
#include "util.hpp"

namespace zf {
namespace i18n {

namespace {

enum class Case : uint8_t { None, Upper, Lower };

// Case of a code point and its counterpart. Latin-1 and Latin Extended-A only: the keys are English; anything else (and
// every other script) counts as uncased and is copied unchanged.
Case case_of(uint32_t c) {
  if (c < 0x80) return c >= 'A' && c <= 'Z' ? Case::Upper : (c >= 'a' && c <= 'z' ? Case::Lower : Case::None);
  if (c >= 0xC0 && c <= 0xDE) return c == 0xD7 ? Case::None : Case::Upper;
  if (c >= 0xDF && c <= 0xFF) return c == 0xF7 ? Case::None : Case::Lower;
  if (c >= 0x100 && c <= 0x17F) {
    if (c == 0x130) return Case::None;
    if (c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return Case::Lower;
    if (c == 0x178) return Case::Upper;
    bool odd_upper = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
    return ((c & 1) != 0) == odd_upper ? Case::Upper : Case::Lower;
  }
  return Case::None;
}

uint32_t to_lower(uint32_t c) {
  if (case_of(c) != Case::Upper) return c;
  if (c < 0x80 || (c >= 0xC0 && c <= 0xDE)) return c + 0x20;
  if (c == 0x178) return 0xFF;
  return c + 1;
}

uint32_t to_upper(uint32_t c) {
  if (case_of(c) != Case::Lower) return c;
  if (c < 0x80 || (c >= 0xE0 && c <= 0xFE)) return c - 0x20;
  if (c == 0xFF) return 0x178;
  if (c == 0xDF || c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return c;  // no single capital
  return c - 1;
}

void put_utf8(std::string& out, uint32_t c) {
  if (c < 0x80) {
    out += char(c);
  } else if (c < 0x800) {
    out += char(0xC0 | (c >> 6));
    out += char(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += char(0xE0 | (c >> 12));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  } else {
    out += char(0xF0 | (c >> 18));
    out += char(0x80 | ((c >> 12) & 0x3F));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  }
}

// Words that keep their own spelling (acronyms, brands, units). Matched on the upper-case ASCII form.
struct Keep {
  const char* upper;
  const char* shown;
};
const Keep kKeep[] = {
    {"WI-FI", "Wi-Fi"}, {"WIFI", "Wi-Fi"}, {"BLUETOOTH", "Bluetooth"}, {"RETROARCH", "RetroArch"}, {"FBNEO", "FBNeo"},
    {"MAME", "MAME"}, {"IP", "IP"}, {"HDMI", "HDMI"}, {"SD", "SD"}, {"USB", "USB"}, {"CPU", "CPU"}, {"GPU", "GPU"},
    {"RGB", "RGB"}, {"LED", "LED"}, {"BIOS", "BIOS"}, {"DNS", "DNS"}, {"SSH", "SSH"}, {"OTA", "OTA"}, {"IA", "IA"}, {"PSP", "PSP"},
    {"GBA", "GBA"}, {"GBC", "GBC"}, {"NES", "NES"}, {"SNES", "SNES"}, {"PS1", "PS1"}, {"PS2", "PS2"}, {"N64", "N64"},
    {"NDS", "NDS"}, {"3DS", "3DS"}, {"RAM", "RAM"}, {"FPS", "FPS"}, {"CRT", "CRT"}, {"LCD", "LCD"}, {"ES", "ES"},
    {"OS", "OS"}, {"ID", "ID"}, {"VM", "VM"}, {"OK", "OK"}, {"UI", "UI"}, {"API", "API"}, {"LAN", "LAN"},
    {"FTP", "FTP"}, {"SMB", "SMB"}, {"URL", "URL"}, {"HTTP", "HTTP"}, {"HTTPS", "HTTPS"}, {"SSID", "SSID"},
    {"WPA", "WPA"}, {"PIN", "PIN"}, {"PC", "PC"}, {"TV", "TV"}, {"DVD", "DVD"}, {"CD", "CD"}, {"NTSC", "NTSC"},
    {"PAL", "PAL"}, {"DOS", "DOS"}, {"SMS", "SMS"}, {"PSX", "PSX"}, {"N/A", "N/A"}, {"DHCP", "DHCP"},
};
// Units after a number: "5GHZ" -> "5GHz".
const Keep kUnits[] = {{"GHZ", "GHz"}, {"MHZ", "MHz"}, {"KHZ", "kHz"}, {"HZ", "Hz"}, {"GB", "GB"}, {"MB", "MB"}, {"KB", "KB"}, {"TB", "TB"}, {"MAH", "mAh"}, {"MS", "ms"}, {"G", "G"}};

bool ieq_upper(std::string_view s, const char* upper) {
  size_t n = std::strlen(upper);
  if (s.size() != n) return false;
  for (size_t i = 0; i < n; ++i) {
    char c = s[i];
    if (c >= 'a' && c <= 'z') c = char(c - 32);
    if (c != upper[i]) return false;
  }
  return true;
}

const char* keep_lookup(std::string_view w) {
  for (const Keep& k : kKeep)
    if (ieq_upper(w, k.upper)) return k.shown;
  return nullptr;
}

bool is_edge_punct(char c) { return std::strchr("([{\"'.,:;!?)]}", c) != nullptr && c != 0; }
bool is_part_sep(char c) { return c == '-' || c == '/' || c == '_' || c == '&' || c == '+' || c == '|'; }

// Lower-cases `w` (all caps) into `out`; the first cased letter is raised when `cap` is set.
void lower_word(std::string_view w, bool cap, std::string& out) {
  if (!w.empty()) {  // only a word that starts with a letter is capitalised ("1080p" stays)
    size_t k = 0;
    cap = cap && case_of(utf8_next(w, k)) != Case::None;
  }
  for (size_t i = 0; i < w.size();) {
    uint32_t c = utf8_next(w, i);
    if (cap && case_of(c) != Case::None) {
      put_utf8(out, to_upper(to_lower(c)));
      cap = false;
    } else {
      put_utf8(out, to_lower(c));
    }
  }
}

// One core word (no edge punctuation): acronym table, then its hyphen/slash parts, then a unit after digits, else lower.
void convert_core(std::string_view core, bool cap, std::string& out) {
  if (const char* k = keep_lookup(core)) {
    out += k;
    return;
  }
  bool split = false;
  for (char c : core) split = split || is_part_sep(c);
  if (split) {
    size_t a = 0;
    bool first = true;
    for (size_t i = 0; i <= core.size(); ++i) {
      if (i == core.size() || is_part_sep(core[i])) {
        std::string_view part = core.substr(a, i - a);
        if (!part.empty()) {
          if (const char* k = keep_lookup(part)) out += k;
          else lower_word(part, cap && first, out);
          first = false;
        }
        if (i < core.size()) out += core[i];
        a = i + 1;
      }
    }
    return;
  }
  size_t d = 0;
  while (d < core.size() && core[d] >= '0' && core[d] <= '9') ++d;
  if (d > 0 && d < core.size())
    for (const Keep& u : kUnits)
      if (ieq_upper(core.substr(d), u.upper)) {
        out += core.substr(0, d);
        out += u.shown;
        return;
      }
  lower_word(core, cap, out);
}

}  // namespace

bool is_all_caps(std::string_view s) {
  bool upper = false;
  for (size_t i = 0; i < s.size();) {
    uint32_t c = utf8_next(s, i);
    if (c == '%') {  // printf conversion ("%d", "%5.1f", "%ls"): its letter is not text
      while (i < s.size() && std::strchr("0123456789.$lhz", s[i]) != nullptr && s[i] != 0) ++i;
      if (i < s.size()) ++i;
      continue;
    }
    Case k = case_of(c);
    if (k == Case::Lower) return false;
    upper = upper || k == Case::Upper;
  }
  return upper;
}

std::string sentence_case(std::string_view s) {
  if (!is_all_caps(s)) return std::string(s);
  std::string out;
  out.reserve(s.size());
  bool cap = true;  // the next word starts a sentence
  size_t i = 0;
  while (i < s.size()) {
    unsigned char ch = static_cast<unsigned char>(s[i]);
    if (ch == ' ' || ch == '\t' || ch == '\n') {
      out += char(ch);
      if (ch == '\n') cap = true;
      ++i;
      continue;
    }
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\n') ++j;
    std::string_view tok = s.substr(i, j - i);
    // printf conversions (%S, %D, %1$S) are copied untouched
    if (tok[0] == '%' && tok.size() > 1) {
      out += tok;
      cap = false;
      i = j;
      continue;
    }
    size_t a = 0, b = tok.size();
    while (a < b && is_edge_punct(tok[a])) ++a;
    while (b > a && is_edge_punct(tok[b - 1])) --b;
    out += tok.substr(0, a);
    if (b > a) convert_core(tok.substr(a, b - a), cap, out);
    out += tok.substr(b);
    cap = b > a ? false : cap;
    // a sentence ends with . ! ? (a trailing colon does not start a new one)
    if (!tok.empty() && (tok.back() == '.' || tok.back() == '!' || tok.back() == '?') && !(tok.size() >= 2 && tok.compare(tok.size() - 2, 2, "..") == 0)) cap = true;
    i = j;
  }
  return out;
}

namespace {
struct Interned {
  std::string src, shown;
};
std::mutex g_mu;
std::deque<Interned> g_store;  // never shrinks: the pointers handed out stay valid
std::unordered_map<std::string_view, const char*>& index() {
  static std::unordered_map<std::string_view, const char*> m;
  return m;
}
}  // namespace

const char* display_text(const char* key) {
  if (!key || !*key) return key;
  size_t n = std::strlen(key);
  if (!is_all_caps(std::string_view(key, n))) return key;  // already mixed case, or nothing cased: as written
  std::lock_guard<std::mutex> g(g_mu);
  auto& idx = index();
  auto it = idx.find(std::string_view(key, n));
  if (it != idx.end()) return it->second;
  g_store.push_back({std::string(key, n), sentence_case(std::string_view(key, n))});
  const Interned& e = g_store.back();
  idx.emplace(std::string_view(e.src), e.shown.c_str());
  return e.shown.c_str();
}

}  // namespace i18n
}  // namespace zf
