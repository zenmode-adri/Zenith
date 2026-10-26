#include "menu_sound.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <memory>

#include "registry.hpp"
#include "settings.hpp"
#include "ui_osd.hpp"
#include "util.hpp"

namespace zf {
namespace sound {

namespace {

// ---- environment ------------------------------------------------------------------------------------------------------
std::string canned_lists(const std::string& cmd) {
  if (cmd == "set-audio list") return "speaker\tInternal speaker\nheadphone\tHeadphones\nhdmi\tHDMI output\n";
  if (cmd == "set-audio list-profiles") return "auto\tAutomatic\nstereo\tAnalog stereo output\nmono\tAnalog mono output\n";
  return {};
}

Env make_env() {
  Env e;
  if (const char* f = std::getenv("ZF_SOUND_FAKE"); f && *f && *f != '0') {
    e.fake = true;
    e.lister = canned_lists;
  }
  if (const char* d = std::getenv("ZF_SOUND_BIN_DIR"); d && *d) {
    e.bin_dir = d;
    if (e.bin_dir.back() != '/') e.bin_dir += '/';
  }
  return e;
}

// ---- volume state (main thread only; the quiet window is the one atomic) --------------------------------------------------
struct Vol {
  int wanted = -1;    // what the user asked for last (-1: not read yet)
  int applied = -1;   // what the last finished `volume N` run set
  bool in_flight = false;
  Refresh refresh;    // of the open sound menu (safe to call after it closed)
};
Vol& vol() {
  static Vol v;
  return v;
}

std::atomic<int64_t> g_quiet_until_ms{0};
int64_t mono_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

bool popup(const Ctx& cx) { return cx.es.get_bool("VolumePopup", true); }

void load_volume(Ctx& cx, bool reread) {
  if (reread && !cx.conf.path().empty()) cx.conf = SystemConf::load(cx.conf.path());
  int v = parse_volume("audio.volume=" + cx.conf.get_or("audio.volume", "100"));
  if (v < 0) v = 100;  // ES: an empty setting means full volume
  Vol& s = vol();
  if (!s.in_flight) s.wanted = s.applied = v;
}

// One `volume N` at a time, always ending on the last value asked: steps that arrive while a run is in flight are folded
// into the next one, so the final volume is right even when the runs would otherwise finish out of order.
void pump(Services* sv) {
  Vol& s = vol();
  if (s.in_flight || s.wanted < 0 || s.wanted == s.applied) return;
  const int target = s.wanted;
  s.in_flight = true;
  quiet_for(4000);  // the script writes audio.volume while it runs
  std::string cmd = "volume " + std::to_string(target);
  sv->jobs.post([sv, cmd] { sv->cmd.run(cmd, 8000); },
                [sv, target] {
                  Vol& v = vol();
                  v.in_flight = false;
                  v.applied = target;
                  quiet_for(800);
                  pump(sv);
                });
}

// ---- the menu --------------------------------------------------------------------------------------------------------------
struct St {
  int threshold = 25;
  std::string dev = "auto", prof = "auto";
  std::vector<Option> devs, profs;
  bool loaded = false;  // the lists came back
};

bool is_auto(const std::string& v) { return v.empty() || v == "auto" || v == "default"; }

/// Lists with the current value included when the script does not report it (ES does the same).
std::vector<Option> with_current(const std::vector<Option>& opts, const std::string& cur) {
  std::vector<Option> v = opts;
  for (const Option& o : v)
    if (o.value == cur) return v;
  v.push_back({cur, cur});
  return v;
}

void load_lists(const std::shared_ptr<St>& st, Services* sv, const Refresh& refresh, bool devices) {
  if (env().fake && env().lister) {  // PC harness (ZF_SOUND_FAKE): canned lists, no thread, so a --preview shows them
    if (devices) st->devs = parse_audio_list(env().lister("set-audio list"), false);
    st->profs = parse_audio_list(env().lister("set-audio list-profiles"), true);
    st->loaded = true;
    if (refresh) refresh();
    return;
  }
  auto out = std::make_shared<std::pair<std::string, std::string>>();
  sv->jobs.post(
      [sv, out, devices] {
        auto run = [sv](const char* c) { return env().lister ? env().lister(c) : sv->cmd.run(c, 8000).out; };
        if (devices) out->first = run("set-audio list");
        out->second = run("set-audio list-profiles");
      },
      [st, out, refresh, devices] {
        if (devices) st->devs = parse_audio_list(out->first, false);
        st->profs = parse_audio_list(out->second, true);
        st->loaded = true;
        if (refresh) refresh();
      });
}

void save_failed(Ctx& cx) { cx.sv->toasts.push(tr("Could not save the setting")); }

void persist(Ctx& cx, const char* key, const std::string& v) {
  if (!cx.conf.persist(key, v)) save_failed(cx);
}

std::unique_ptr<Screen> make_sound(Ctx& cx) {
  load_volume(cx, true);  // hardware keys write audio.volume behind our back: start from the file
  auto st = std::make_shared<St>();
  {
    long t = 0;
    const std::string* p = cx.conf.get("system.battery.warning_threshold");
    st->threshold = p && parse_int(*p, t) ? int(std::min(std::max(t, 0L), 50L)) : 25;
    st->dev = is_auto(cx.conf.get_or("audio.device", "")) ? "auto" : cx.conf.get_or("audio.device", "");
    st->prof = is_auto(cx.conf.get_or("audio.profile", "")) ? "auto" : cx.conf.get_or("audio.profile", "");
  }

  MenuSpec s;
  s.title = tr("SOUND SETTINGS");
  s.title_key = N_("SOUND SETTINGS");
  s.build = [st](MenuBuilder& m, Ctx& cx) {
    const bool has_volume = have_tool("volume");
    const bool has_output = have_tool("set-audio") && st->loaded && (!st->devs.empty() || !st->profs.empty());
    if (has_volume || has_output) m.header(tr("VOLUME"));
    if (has_volume) {
      m.slider(tr("SYSTEM VOLUME"), 0, 100, 1, [&cx] { return volume(cx); }, [&cx](int v) { set_volume(cx, v); }).unit("%");
      m.toggle(tr("SHOW OVERLAY WHEN VOLUME CHANGES"), [&cx] { return popup(cx); }, [&cx](bool b) {
        cx.es.set_bool("VolumePopup", b);
        std::string err;
        if (!cx.es.save(&err)) save_failed(cx);
      });
    }
    if (has_output) {
      Refresh refresh = m.refresh;
      auto change = [st, &cx, refresh](bool profile, const std::string& v) {
        std::string& cur = profile ? st->prof : st->dev;
        const std::string prev = cur;
        cur = v;
        Services* sv = cx.sv.get();
        Ctx* c = &cx;
        auto code = std::make_shared<int>(-1);
        std::string cmd = std::string("set-audio ") + (profile ? "set-profile " : "set ") + shell_quote(v);
        sv->jobs.post([sv, cmd, code] { *code = sv->cmd.run(cmd, 8000).exit_code; },
                      [st, c, profile, v, prev, code, refresh, sv] {
                        std::string& cur2 = profile ? st->prof : st->dev;
                        if (*code != 0) {  // the script failed: show what is really selected
                          cur2 = prev;
                          if (refresh) refresh();
                          return;
                        }
                        if (profile) {
                          // ES: "auto"/"default" delete the line from system.cfg
                          if (is_auto(v)) {
                            if (!c->conf.persist_remove("audio.profile")) save_failed(*c);
                          } else {
                            persist(*c, "audio.profile", v);
                          }
                        } else {
                          persist(*c, "audio.device", v);  // audio.device keeps even "auto" (ES: dontRemoveValue)
                          load_lists(st, sv, refresh, false);  // the profiles on offer depend on the output
                        }
                      });
      };
      if (!st->devs.empty())
        m.choice(tr("AUDIO OUTPUT"), with_current(st->devs, st->dev), [st] { return st->dev; },
                 [change](const std::string& v) { change(false, v); })
            .no_cycle();
      if (!st->profs.empty())
        m.choice(tr("AUDIO PROFILE"), with_current(st->profs, st->prof), [st] { return st->prof; },
                 [change](const std::string& v) { change(true, v); })
            .no_cycle()
            .hint(tr("Available options can change depending on current audio output."));
    }

    m.header(tr("SOUNDS"));
    m.toggle(tr("ENABLE AUDIBLE BATTERY WARNING"), [&cx] { return cx.conf.get_bool("system.battery.warning", false); },
             [&cx](bool b) { persist(cx, "system.battery.warning", b ? "1" : "0"); });
    m.slider(tr("BATTERY WARNING THRESHOLD"), 0, 50, 1, [st] { return st->threshold; }, [st](int v) { st->threshold = v; })
        .unit("%")
        .commit([st, &cx](int v) { persist(cx, "system.battery.warning_threshold", std::to_string(v)); });
  };
  s.on_open = [st](Ctx& cx, const Refresh& r) {
    vol().refresh = r;
    if (have_tool("set-audio")) load_lists(st, cx.sv.get(), r, true);
  };
  s.on_close = [](Ctx&) { vol().refresh = nullptr; };
  return make_menu(std::move(s));
}

}  // namespace

// ---- public ----------------------------------------------------------------------------------------------------------------
Env& env() {
  static Env e = make_env();
  return e;
}

bool have_tool(std::string_view name) {
  const Env& e = env();
  if (e.fake) return true;
  return ::access((e.bin_dir + std::string(name)).c_str(), F_OK) == 0;
}

void set_volume(Ctx& cx, int percent) {
  percent = std::min(std::max(percent, 0), 100);
  Vol& s = vol();
  if (s.wanted < 0) load_volume(cx, false);
  s.wanted = percent;
  if (popup(cx)) osd_show_level(cx, "volume", float(percent) / 100.0f);
  pump(cx.sv.get());
}

void change_volume(Ctx& cx, int delta) { set_volume(cx, volume(cx) + delta); }

int volume(Ctx& cx) {
  if (vol().wanted < 0) load_volume(cx, false);
  return vol().wanted;
}

void external_volume(Ctx& cx, int percent, bool fresh) {
  percent = std::min(std::max(percent, 0), 100);
  Vol& s = vol();
  if (!s.in_flight) s.wanted = s.applied = percent;
  if (s.refresh) s.refresh();  // an open sound menu moves its slider
  if (fresh && popup(cx)) osd_show_level(cx, "volume", float(percent) / 100.0f);
}

void quiet_for(int ms) { g_quiet_until_ms.store(mono_ms() + ms, std::memory_order_relaxed); }
bool quiet() { return mono_ms() < g_quiet_until_ms.load(std::memory_order_relaxed); }

void reset_state() {
  vol() = Vol();
  g_quiet_until_ms.store(0);
  osd_clear();
}

int parse_volume(std::string_view cfg_text) {
  SystemConf c = SystemConf::from_text(std::string(cfg_text));
  const std::string* p = c.get("audio.volume");
  long v = 0;
  if (!p || !parse_int(*p, v) || v < 0) return -1;
  return int(std::min(v, 100L));
}

Seen decide(VolumeTracker& tracker, std::string_view cfg_text, bool is_quiet, bool game_running, int& volume) {
  int v = tracker.update(parse_volume(cfg_text));
  if (v < 0 || is_quiet) return Seen::Nothing;  // the baseline moved even when we ignore it
  volume = v;
  return game_running ? Seen::Sync : Seen::Osd;
}

std::vector<Option> parse_audio_list(std::string_view text, bool profiles) {
  std::vector<Option> out;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (line.empty()) continue;
    std::vector<std::string> tok;
    size_t a = 0;
    for (;;) {
      size_t t = line.find('\t', a);
      tok.emplace_back(line.substr(a, t == std::string_view::npos ? std::string_view::npos : t - a));
      if (t == std::string_view::npos) break;
      a = t + 1;
    }
    Option o;
    o.value = tok[0];
    if (tok.size() >= 2) {
      size_t first = 1;
      if (profiles && tok[1].find("bluez_card") != std::string::npos) first = 2;  // ES drops the card id from the name
      for (size_t i = first; i < tok.size(); ++i) {
        if (i > first) o.label += ' ';
        o.label += tok[i];
      }
    }
    if (o.label.empty()) o.label = o.value;
    out.push_back(std::move(o));
  }
  return out;
}

}  // namespace sound

ZF_REGISTER_SCREEN("menu.sound", [](Ctx& cx, const std::string&) { return sound::make_sound(cx); });

}  // namespace zf
