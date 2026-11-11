#include "uimode.hpp"

#include <atomic>

#include "collections.hpp"

namespace zf::uimode {

namespace {
std::atomic<int> g_mode{int(Mode::Full)};

const char* name_of(Mode m) { return m == Mode::Kid ? "Kid" : m == Mode::Kiosk ? "Kiosk" : "Full"; }
}  // namespace

Mode of(const EsSettings& es) {
  const bool force_kid = es.get_bool("ForceKid", false), force_kiosk = es.get_bool("ForceKiosk", false);
  const std::string* s = es.get("UIMode");
  const std::string v = s ? *s : std::string("Full");
  if (force_kid || (v == "Kid" && !force_kiosk)) return Mode::Kid;
  if (force_kiosk || v == "Kiosk") return Mode::Kiosk;
  if (v == "Full" || v.empty()) return Mode::Full;
  return Mode::Kiosk;  // "Basic" or something unknown: never open
}

void apply(const EsSettings& es) { g_mode = int(of(es)); }
Mode current() { return Mode(g_mode.load()); }

bool screen_allowed(Mode mode, std::string_view name) {
  if (mode == Mode::Full) return true;
  for (std::string_view ok : {"menu.main", "menu.system.info", "menu.quit", "menu.uimode.unlock", "menu.savestates", "menu.controllers.detect"})
    if (name == ok) return true;
  return false;
}

std::string passkey(const EsSettings& es) {
  const std::string* s = es.get("UIMode_passkey");
  return s && !s->empty() ? *s : std::string("aaaba");
}

std::string passkey_text(const std::string& seq) {
  std::string out;
  for (char c : seq) {
    const char* t = nullptr;
    switch (c) {
      case 'u': t = "\xE2\x86\x91"; break;  // U+2191
      case 'd': t = "\xE2\x86\x93"; break;  // U+2193
      case 'l': t = "\xE2\x86\x90"; break;  // U+2190
      case 'r': t = "\xE2\x86\x92"; break;  // U+2192
      case 'a': t = "A"; break;
      case 'b': t = "B"; break;
      case 'x': t = "X"; break;
      case 'y': t = "Y"; break;
      default: break;
    }
    if (!t) continue;  // not a key of the code: nothing to show
    if (!out.empty()) out += ", ";
    out += t;
  }
  return out;
}

bool Unlock::feed(Action a, bool invert) {
  char c = 0;
  switch (a) {
    case Action::Up: c = 'u'; break;
    case Action::Down: c = 'd'; break;
    case Action::Left: c = 'l'; break;
    case Action::Right: c = 'r'; break;
    case Action::A: c = invert ? 'a' : 'b'; break;
    case Action::B: c = invert ? 'b' : 'a'; break;
    case Action::X: c = 'x'; break;
    case Action::Y: c = 'y'; break;
    default: break;  // Start, Select, shoulders...: a mapped input that is not part of any code
  }
  if (c != 0 && n_ < seq_.size() && seq_[n_] == c)
    ++n_;
  else
    n_ = 0;
  if (!seq_.empty() && n_ == seq_.size()) {
    n_ = 0;
    return true;
  }
  return false;
}

bool set_mode(EsSettings& es, Mode mode) {
  es.set_string("UIMode", name_of(mode));
  const bool ok = es.save();
  apply(es);
  collections::request_rescan();  // the lists change with the mode (kid games only, systems without games)
  return ok;
}

}  // namespace zf::uimode
