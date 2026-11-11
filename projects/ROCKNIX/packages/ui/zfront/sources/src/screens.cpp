#include "listopts.hpp"
#include "screens.hpp"

#include <chrono>

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>

#include "collections.hpp"
#include "colsys.hpp"
#include "covers.hpp"
#include "detailed.hpp"
#include "gamemeta.hpp"
#include "i18n.hpp"
#include "launch.hpp"
#include "launchcheck.hpp"
#include "remake.hpp"
#include "oscontract.hpp"
#include "savestates.hpp"
#include "sound.hpp"
#include "scene.hpp"
#include "ui_dialogs.hpp"
#include "uimode.hpp"
#include "util.hpp"
#include "video.hpp"
#include "vpreview.hpp"

namespace zf {

namespace {
// The system whose gamelist and ROM folder hold the details (cover, description) of `g`, a game of `sys`: for a collection's copy, the
// system it belongs to (found without reading anything); otherwise `sys` itself.
const System& details_sys(const Ctx& cx, const System& sys, const Game& g) {
  if (sys.collection)
    if (const System* src = colsys::source_system(cx, g)) return *src;
  return sys;
}
}  // namespace

/// "12 min ago" for a `GameMeta::lastplayed` (YYYYMMDDHHMMSS, local time); empty when never or older than 30 days.
// A tile of the "continue playing" row needs its ROM: a state left behind by a deleted ROM would give an A that does nothing.
// The usual flat folder costs one stat per extension; anything else (sub folder, upper-case extension) asks the game list.
bool rom_in_folder(const System& sys, const std::string& stem) {
  for (const std::string& e : sys.exts) {
    struct stat st;
    if (::stat(path_join(sys.rom_dir, stem + e).c_str(), &st) == 0) return true;
  }
  // an extension in the other case ("Game.GB"): look at the names of the folder instead of loading the whole gamelist on the thread that draws
  std::vector<std::string> want;
  for (const std::string& e : sys.exts) want.push_back(utf8_lower(stem + e));
  DIR* d = ::opendir(sys.rom_dir.c_str());
  if (!d) return false;
  bool found = false;
  while (!found) {
    const dirent* de = ::readdir(d);
    if (!de) break;
    if (de->d_name[0] == '.') continue;
    const std::string low = utf8_lower(de->d_name);
    found = std::find(want.begin(), want.end(), low) != want.end();
  }
  ::closedir(d);
  return found;
}

size_t list_page_rows(size_t lines, size_t shown) { return std::max<size_t>(1, shown ? shown : lines); }

static bool rom_present(Ctx& cx, size_t si, const std::string& stem) {
  size_t game = 0;
  // Kid / Kiosk: the game list is what the mode lets the person see (kid games only, hidden games out), so a ROM that is on the card
  // but not in the list must not show its picture here (the same rule as `resume::newest`)
  if (uimode::current() != uimode::Mode::Full) return savestates::find_game(cx, si, stem, game);
  if (rom_in_folder(cx.systems[si], stem)) return true;
  return savestates::find_game(cx, si, stem, game);  // a sub folder (or the ROM is gone): the gamelist knows
}

// What a tile of the row says under its picture: "12 min ago", plus " - GBA" when the row mixes the systems (Zenith Next).
static std::string tile_ago(const Ctx& cx, const RecentItem& it, std::time_t now) {
  std::string ago = ago_text(it.mtime, now);
  if (it.sys < 0 || size_t(it.sys) >= cx.systems.size()) return ago;
  std::string tag = cx.systems[size_t(it.sys)].name;
  for (char& c : tag) c = char(std::toupper(static_cast<unsigned char>(c)));
  return ago.empty() ? tag : ago + " - " + tag;
}

bool CarouselScreen::refresh_ago(const Ctx& cx, std::time_t now) {
  if (recent_at_ != 0 && now / 60 == recent_at_ / 60) return false;  // the words only change when the minute does
  recent_at_ = now;
  bool any = false;
  for (RecentItem& it : recent_.items) {
    if (it.mtime == 0) continue;  // a hand-made item has no time to count from
    const std::string s = tile_ago(cx, it, now);
    if (s != it.ago) it.ago = s, any = true;
  }
  return any;
}

void CarouselScreen::restore_focus(const Ctx& cx) {
  if (resume_name_.empty()) return;
  int at = -1;  // a game that no longer has its tile gives the keys back to the carousel instead of leaving them on another game
  for (size_t i = 0; i < recent_.items.size() && at < 0; ++i)
    if (recent_.items[i].name == resume_name_ && (recent_.items[i].sys < 0 || recent_.items[i].sys == resume_sys_)) at = int(i);
  set_focus(at, cx);
  resume_name_.clear();
  resume_sys_ = -1;
}

static std::string played_ago(uint64_t lp, std::time_t now) {
  if (lp == 0) return std::string();
  std::tm t{};
  t.tm_year = int(lp / 10000000000ULL) - 1900;
  t.tm_mon = int(lp / 100000000ULL % 100) - 1;
  t.tm_mday = int(lp / 1000000ULL % 100);
  t.tm_hour = int(lp / 10000ULL % 100);
  t.tm_min = int(lp / 100ULL % 100);
  t.tm_sec = int(lp % 100);
  t.tm_isdst = -1;
  return ago_text(std::mktime(&t), now);
}

// Group of a game for L2 / R2: its first character, upper case ("atom" and "Alpha" are both "A").
static std::string letter_key(const std::string& name) {
  if (name.empty()) return std::string();
  size_t i = 0;
  std::string k;
  utf8_append(k, utf8_next(name, i));
  return utf8_upper(k);
}

static bool g_random_pending = false;  // set by RANDOM GAME, applied by the game list (take_search)
void request_random_game() { g_random_pending = true; }

bool CarouselScreen::row_live(const Ctx& cx) const {
  if (remake::next(cx.th)) return recent_for_ == kAllSystems && !recent_.items.empty();  // one row for every system: it never moves
  const long n = long(cx.items.size());
  const long idx = n > 0 ? ((long(target_) % n) + n) % n : -1;
  return !anim_ && idx >= 0 && idx == recent_for_ && idx == info_idx_;  // the row belongs to the system the carousel stopped on
}

std::vector<size_t> CarouselScreen::row_tiles(Ctx& cx) {
  const long n = long(cx.items.size());
  if (n == 0) return {};
  const long idx = ((long(target_) % n) + n) % n;
  return recent_visible(cx.th, cx.assets, cx.w, cx.h, cx.items[size_t(idx)], recent_);
}

void CarouselScreen::set_focus(int item, const Ctx& cx) {
  focus_ = item;
  if (item >= 0) {
    ring_idx_ = item;
    if (!cx.ui_motion) ring_a_ = 1.0f;
  } else if (!cx.ui_motion) {
    ring_a_ = 0.0f;
    ring_idx_ = -1;
  }
}

Transition CarouselScreen::launch_recent(Ctx& cx) {
  if (focus_ < 0 || size_t(focus_) >= recent_.items.size() || !row_live(cx)) return Transition::none();
  const RecentItem it = recent_.items[size_t(focus_)];
  const size_t sys = it.sys >= 0 ? size_t(it.sys) : size_t(recent_for_);
  size_t game = 0;
  if (sys >= cx.systems.size() || !savestates::find_game(cx, sys, it.name, game)) {  // the ROM is gone (or the mode hides the game)
    cx.sv->toasts.push(tr("GAME NOT FOUND"), 2500);
    resume_name_ = it.name;  // the row is read again: the tile goes and the keys return to the carousel
    resume_sys_ = long(sys);
    reread_ = true;
    return Transition::none();
  }
  // the same launch as picking that state in the savestate manager (BIOS question included)
  Transition t = savestates::launch_state(cx, sys, game, it.slot);
  if (t.kind != Transition::None) {  // the game starts, or the BIOS question comes first (YES starts it, NO changes nothing)
    resume_name_ = it.name;          // when it ends the row is read again (new capture, new order) and the keys return to its tile
    resume_sys_ = long(sys);
    reread_ = true;
  }
  if (t.kind == Transition::Launch) {
    focus_ = ring_idx_ = -1;
    ring_a_ = 0.0f;
    recent_for_ = -1;
  }
  return t;
}

constexpr float kWaveFps = 30.0f;  // slow drift: more would only cost battery

Transition CarouselScreen::handle(Action a, Ctx& cx) {
  int n = int(cx.items.size());
  if (focus_ >= 0 && remake::cross(cx.th)) {  // a column: up and down walk it, up at the top or a step sideways go back to the systems
    const std::vector<size_t> vis = row_tiles(cx);
    if (a == Action::Down) a = Action::Right;
    else if (a == Action::Up && !vis.empty() && vis.front() != size_t(focus_)) a = Action::Left;
    else if (a == Action::Left || a == Action::Right) set_focus(-1, cx);
  }
  if (focus_ >= 0) {  // the keys are on the "continue playing" row
    switch (a) {
      case Action::Left:
      case Action::Right: {
        const std::vector<size_t> vis = row_tiles(cx);
        auto at = std::find(vis.begin(), vis.end(), size_t(focus_));
        if (vis.empty()) {
          set_focus(-1, cx);
        } else if (at == vis.end()) {
          set_focus(int(vis.front()), cx);
        } else if (a == Action::Left && at != vis.begin()) {  // no wrap: a short row, a stray press must not jump
          set_focus(int(*(at - 1)), cx);
        } else if (a == Action::Right && at + 1 != vis.end()) {
          set_focus(int(*(at + 1)), cx);
        }
        return Transition::none();
      }
      case Action::A:
        return launch_recent(cx);
      case Action::B:
      case Action::Up:
        set_focus(-1, cx);
        return Transition::none();
      case Action::L2:
      case Action::R2:  // the triggers keep stepping the carousel: the keys go back to it
        set_focus(-1, cx);
        break;
      default:  // Start (menu) and the rest as on the carousel
        break;
    }
  }
  switch (a) {
    case Action::Down:  // the row is under the carousel
      if (focus_ < 0 && row_live(cx)) {
        const std::vector<size_t> vis = row_tiles(cx);
        if (!vis.empty()) set_focus(int(vis.front()), cx);
      }
      break;
    case Action::Right:
    case Action::R2:  // ES: the triggers step the carousel like left/right
      if (n > 0) {
        ++target_;
        anim_ = true;
        sound::play(cx, sound::Cue::Scroll);
      }
      break;
    case Action::Left:
    case Action::L2:
      if (n > 0) {
        --target_;
        anim_ = true;
        sound::play(cx, sound::Cue::Scroll);
      }
      break;
    case Action::A:
      if (n > 0 && std::fabs(float(target_) - cam_) < kGlideAcceptA) {
        size_t idx = size_t(((target_ % n) + n) % n);
        recent_for_ = -1;
        announced_ = -1;  // coming back announces the system again, as ES does when it returns to the system view
        return Transition::push(std::make_unique<ListScreen>(cx.system_of(idx), cx));
      }
      break;
    case Action::X:  // ES: a random system
      if (n > 1) {
        const int cur = int(system_cursor(cx));
        int pick = std::uniform_int_distribution<int>(0, n - 2)(rng_);
        if (pick >= cur) ++pick;
        target_ += pick - cur;
        anim_ = true;
        sound::play(cx, sound::Cue::Scroll);
      }
      break;
    case Action::Select:  // ES: the quick access menu (screensaver, quit options)
      return Transition::open("menu.quit", "quick");
    case Action::Start:
      return Transition::open("menu.main");
    default:
      break;
  }
  return Transition::none();
}

long CarouselScreen::system_cursor(const Ctx& cx) const {
  const long n = long(cx.items.size());
  return n > 0 ? ((long(target_) % n) + n) % n : -1;
}

void CarouselScreen::reset_system_cursor(Ctx& cx, long item) {
  target_ = int(item);
  cam_ = float(item);
  anim_ = false;
  info_idx_ = -1;  // the text under the carousel is built again for the new system, without a fade
  info_a_ = 1.0f;
  announced_ = -1;
  recent_ = RecentRow();
  recent_for_ = -1;
  set_focus(-1, cx);  // the tiles of the old systems are gone: the keys are on the carousel
  resume_name_.clear();
}

bool CarouselScreen::tick(float dt, Ctx& cx) {
  waves_ = remake::cross(cx.th) && cx.ui_motion;
  if (waves_) {
    wave_left_ -= dt;
    if (wave_left_ <= 0.0f) {
      wave_left_ = 1.0f / kWaveFps;
      wave_redraw_ = true;
    }
  }
  if (!anim_ && !cx.items.empty()) {  // `system-selected` once the carousel stopped on a system (ES: CURSOR_STOPPED)
    long n = long(cx.items.size());
    long idx = ((long(target_) % n) + n) % n;
    if (idx != announced_) {
      announced_ = idx;
      const size_t si = cx.system_of(size_t(idx));
      if (si < cx.systems.size() && oscontract::selected_listening("system-selected")) oscontract::fire_system_selected(cx.systems[si].name);
    }
  }
  if (anim_) {
    const float to = float(target_);
    // ES MoveCarousel (CAROUSEL TRANSITIONS, default on): off = the camera jumps to the card
    cam_ = cx.es.get_bool("MoveCarousel", true) ? cam_ + (to - cam_) * (1.0f - std::exp(-kGlideRate * dt)) : to;
    if (std::fabs(to - cam_) < kGlideEnd) {
      anim_ = false;
      cam_ = to;
    }
  }
  bool changed = false;
  if (!cx.items.empty()) {
    const long n = long(cx.items.size());
    const long cur = ((long(target_) % n) + n) % n;
    if (!cx.ui_motion || info_idx_ < 0 || info_idx_ >= n) {
      info_idx_ = cur;
      info_a_ = 1.0f;
    } else if (cur != info_idx_) {  // leaving the old text: quick
      info_a_ = std::max(0.0f, info_a_ - dt / kInfoOut);
      if (info_a_ <= 0.0f) info_idx_ = cur;
      changed = true;
    } else if (info_a_ < 1.0f) {  // the new text comes in as the camera settles
      if (std::fabs(float(target_) - cam_) < kInfoInAt) info_a_ = std::min(1.0f, info_a_ + dt / kInfoIn);
      changed = true;
    }
  }
  if (remake::next(cx.th) && !cx.systems.empty() && (recent_for_ != kAllSystems || reread_)) {
    // Zenith Next: the last games of every system, newest first, read once (and again after a game started from the row), so the row
    // and the card above it stay where they are while the carousel moves
    reread_ = false;
    recent_for_ = kAllSystems;
    constexpr size_t kTiles = 3;
    const time_t now = std::time(nullptr);
    std::vector<RecentItem> all;
    for (size_t si = 0; si < cx.systems.size(); ++si) {
      if (cx.item_of(si) < 0) continue;  // a hidden system: the carousel does not show it, so neither does the row (as `resume::newest`)
      std::vector<RecentItem> v = recent_states(cx.systems[si], now, kTiles * 3);  // spare ones replace the tiles without ROM
      size_t kept = 0;
      for (RecentItem& it : v) {
        if (kept == kTiles || !rom_present(cx, si, it.name)) continue;
        it.sys = long(si);
        it.ago = tile_ago(cx, it, now);  // which console it is
        all.push_back(std::move(it));
        ++kept;
      }
    }
    std::stable_sort(all.begin(), all.end(), [](const RecentItem& x, const RecentItem& y) { return x.mtime > y.mtime; });
    if (all.size() > kTiles) all.resize(kTiles);
    recent_.items = std::move(all);
    recent_at_ = now;
    changed = true;
    restore_focus(cx);  // back from a game started on the row: the keys return to the tile of that game
  }
  if (!remake::next(cx.th) && !anim_ && !cx.items.empty()) {  // the "continue playing" row is read once the carousel stopped, not while it moves
    if (reread_) recent_for_ = -1, reread_ = false;  // a game was started from the row (its dialog or its run is over now)
    long n = long(cx.items.size());
    long idx = ((long(target_) % n) + n) % n;
    if (idx != recent_for_) {
      recent_for_ = idx;
      const size_t si = cx.system_of(size_t(idx));
      recent_.items.clear();
      if (si < cx.systems.size() && (!remake::active(cx.th) || remake::resumes(cx.th))) {
        constexpr size_t kTiles = 3;
        recent_.items = recent_states(cx.systems[si], std::time(nullptr), kTiles * 3);  // spare ones replace the tiles without ROM
        recent_.items.erase(std::remove_if(recent_.items.begin(), recent_.items.end(), [&](const RecentItem& it) { return !rom_present(cx, si, it.name); }),
                            recent_.items.end());
        if (recent_.items.size() > kTiles) recent_.items.resize(kTiles);
      }
      recent_at_ = std::time(nullptr);
      changed = true;
      restore_focus(cx);  // back from a game started on the row: the keys return to the tile of that game
    }
  }
  if (refresh_ago(cx, std::time(nullptr))) changed = true;  // "1 min ago" does not stay "1 min ago" for an hour
  if (focus_ >= 0 && !row_live(cx)) set_focus(-1, cx);  // the row went away (the carousel moved, a menu changed the language...)
  const float ring_to = focus_ >= 0 ? 1.0f : 0.0f;
  if (ring_a_ != ring_to) {  // focus ring fade (without motion `set_focus` already put it there)
    const float step = cx.ui_motion ? dt / kRingFade : 1.0f;
    ring_a_ = ring_to > ring_a_ ? std::min(ring_to, ring_a_ + step) : std::max(ring_to, ring_a_ - step);
    if (ring_a_ <= 0.0f) ring_idx_ = -1;
    changed = true;
  }
  if (remake::next(cx.th)) {  // the card goes to the middle of the screen where there is no row to show, and comes back
    const float to = row_tiles(cx).empty() ? 0.0f : 1.0f;
    if (room_ < 0.0f || !cx.ui_motion) {
      changed = changed || room_ != to;
      room_ = to;
      room_v_ = 0.0f;
    } else if (room_ != to || room_v_ != 0.0f) {
      // a critically damped spring (no overshoot): soft start, soft stop, about 0.35 s; small steps so a long frame stays stable
      constexpr float kOmega = 16.0f;
      for (float left = dt; left > 0.0f; left -= 1.0f / 240.0f) {
        const float h = std::min(left, 1.0f / 240.0f);
        room_v_ += (-kOmega * kOmega * (room_ - to) - 2.0f * kOmega * room_v_) * h;
        room_ += room_v_ * h;
      }
      if (std::fabs(room_ - to) < 0.002f && std::fabs(room_v_) < 0.02f) room_ = to, room_v_ = 0.0f;
      room_ = std::clamp(room_, 0.0f, 1.0f);
      changed = true;
    }
  }
  return anim_ || changed;
}

void CarouselScreen::draw(Ctx& cx, std::vector<Op>& out) {
  if (remake::cross(cx.th)) {
    static const auto t0 = std::chrono::steady_clock::now();
    if (cx.ui_motion) remake::state().wave_t = std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
  }
  const bool show = row_live(cx);
  {
    remake::state().row_room = room_ < 0.0f ? 1.0f : room_;  // the spring already eases it
  }
  recent_.decode_inline = !cx.async_covers;
  recent_.focus = show ? ring_idx_ : -1;
  recent_.focus_a = ring_a_;
  recent_.focus_help = show && focus_ >= 0;
  remake::state().inline_decode = !cx.async_covers;
  remake::state().pending.clear();
  system_view(cx.th, cx.assets, cx.common, cx.w, cx.h, cx.items, target_, cam_, cx.status, show ? &recent_ : nullptr, InfoFade{int(info_idx_), info_a_}, out);
  if (!remake::state().pending.empty()) covers::request(cx.assets, cx.sv->jobs, remake::state().pending, remake::state().pending_w, remake::state().pending_h);
  if (show && recent_.box_w > 0 && cx.async_covers) {
    // the decoder keeps only the newest request: ask for one screenshot per frame, the next one when this one lands
    for (const RecentItem& it : recent_.items) {
      ImgId id;
      const std::string key = it.shot_key();
      if (covers::failed(key) || covers::ready(cx.assets, key, recent_.box_w, recent_.box_h, id)) continue;
      covers::request(cx.assets, cx.sv->jobs, key, recent_.box_w, recent_.box_h);
      break;
    }
  }
  if (show && focus_ >= 0) {  // the tile with the keys failed to decode and is not drawn: they go to the last one that is
    const std::vector<size_t> vis = row_tiles(cx);
    if (std::find(vis.begin(), vis.end(), size_t(focus_)) == vis.end()) set_focus(vis.empty() ? -1 : int(vis.back()), cx);
  }
}

namespace {
/// Where the cursor of each system's list was left, per `Ctx` (its assets' uid) and system: the path of the game.
std::map<std::pair<uint64_t, std::string>, std::string>& cursor_memory() {
  static std::map<std::pair<uint64_t, std::string>, std::string> m;
  return m;
}
}  // namespace

void ListScreen::save_cursor(const Ctx& cx) const {
  if (si_ >= cx.sys_total() || cur_path_.empty() || !query_.empty()) return;  // a search shows other rows: not where the user was
  cursor_memory()[{cx.assets.uid(), cx.sys_at(si_).name}] = cur_path_;
}

void ListScreen::restore_cursor(const Ctx& cx) {
  if (si_ >= cx.sys_total() || !query_.empty()) return;
  auto it = cursor_memory().find({cx.assets.uid(), cx.sys_at(si_).name});
  if (it == cursor_memory().end()) return;
  const auto& games = cx.sys_at(si_).games;
  for (size_t r = 0; r < rows_.size(); ++r)
    if (games[rows_[r]].path == it->second) {
      cur_ = r;
      return;
    }
}

ListScreen::ListScreen(size_t system_index, Ctx& cx) : si_(system_index), lines_(remake::active(cx.th) ? 8 : size_t(cx.th.f("listLines"))), settle_(kSettleSeconds) {
  listopts::prepare(cx, si_);  // hidden games and the article rule are decided when the games are read
  opts_gen_ = listopts::generation();
  cx.open_system(si_);  // the gamelist is read when the user enters the system
  g_random_pending = false;  // a request left by an earlier list's options must not move this one
  refill(cx);
  gen_ = games_generation();
  restore_cursor(cx);  // before `remember`, which would take the first row for where the user was
  remember(cx);
  if (!names_.empty() && oscontract::selected_listening("game-selected")) sel_wait_ = 0;  // the first game counts as selected on entry
}

void ListScreen::remember(const Ctx& cx) {
  const auto& games = cx.sys_at(si_).games;
  cur_path_ = cur_ < rows_.size() && rows_[cur_] < games.size() ? games[rows_[cur_]].path : std::string();
}

void ListScreen::sync_games(Ctx& cx) {
  if (gen_ == games_generation()) return;
  gen_ = games_generation();
  colsys::resync(cx, si_);  // a collection: current metadata, and the games that stopped being members leave
  refill(cx);
  const auto& games = cx.sys_at(si_).games;
  size_t at = rows_.size();
  for (size_t r = 0; r < rows_.size() && !cur_path_.empty(); ++r)
    if (games[rows_[r]].path == cur_path_) {
      at = r;
      break;
    }
  if (at < rows_.size()) cur_ = at;
  else if (cur_ >= rows_.size()) cur_ = rows_.empty() ? 0 : rows_.size() - 1;
  remember(cx);
  view_ = -1.0f;  // no slide from the row of the old list
  d_valid_ = false;
  redraw_ = true;
}

void ListScreen::refill(Ctx& cx) {
  const System& sys = cx.sys_at(si_);
  const auto& games = sys.games;
  const std::time_t now = std::time(nullptr);
  const listopts::Opts o = listopts::read(cx.es, sys.name);
  const bool collection = sys.name == "favorites" || sys.name == "recent" || (sys.collection && colsys::keeps_order(sys.name));  // ES: no favourites on top there
  rows_ = listopts::arrange(games, o, listopts::filter_of(sys.name), utf8_lower(query_), collection);
  names_.clear();
  marks_.clear();
  rowimg_.assign(rows_.size(), std::string());
  rowimg_have_.assign(rows_.size(), 0);
  prefetch_reset();
  for (size_t i : rows_) {
    names_.push_back(listopts::display_name(games[i], o));
    marks_.push_back(played_ago(games[i].meta.lastplayed, now));
  }
}

void ListScreen::sync_opts(Ctx& cx) {
  if (opts_gen_ == listopts::generation()) return;
  opts_gen_ = listopts::generation();
  listopts::prepare(cx, si_);
  refill(cx);
  cur_ = 0;
  remember(cx);
  view_ = -1.0f;
  settle_ = 0;
  d_valid_ = false;
  redraw_ = true;
  if (!names_.empty() && oscontract::selected_listening("game-selected")) sel_wait_ = 0;
}

void ListScreen::search(Ctx& cx, const std::string& text) {
  query_ = text;
  refill(cx);
  cur_ = 0;
  remember(cx);
  view_ = -1.0f;  // no slide from the row of the old list
  settle_ = 0;
  d_valid_ = false;
  redraw_ = true;
  if (!names_.empty() && oscontract::selected_listening("game-selected")) sel_wait_ = 0;
}

void ListScreen::prefetch_step(Ctx& cx, const std::string& title) {
  remake::DrawState& rs = remake::state();
  const System& sys = cx.sys_at(si_);
  while (pf_k_ < 2 * kPrefetchRows) {
    const size_t dist = pf_k_ / 2 + 1;  // +1, -1, +2, -2: the nearest first
    const bool forward = pf_k_ % 2 == 0;
    if (forward ? cur_ + dist >= rows_.size() : dist > cur_) {
      ++pf_k_;
      continue;
    }
    const size_t row = forward ? cur_ + dist : cur_ - dist;
    DetailedData next;
    if (rows_[row] >= sys.games.size() || !detailed_for(details_sys(cx, sys, sys.games[rows_[row]]), sys.games[rows_[row]], true, next)) {
      ++pf_k_;
      continue;
    }
    next.decode_inline = false;
    (void)row_picture(cx, row);
    // The neighbour is drawn into a scrap: what its cover, description and name need (their pictures asked of the job thread, their text
    // made) is then in the caches when the cursor gets there. Nothing of it reaches the screen.
    rs.pending.clear();
    std::vector<Op> scrap;
    list_view(cx.th, cx.assets, cx.common, cx.w, cx.h, title, names_, cur_, cx.status, &next, &marks_, scrap);
    std::string want;
    uint32_t want_w = 0, want_h = 0;
    if (!rs.pending.empty()) {
      want = rs.pending, want_w = rs.pending_w, want_h = rs.pending_h;
    } else if (next.cover_missing) {
      want = next.image, want_w = next.cover_w, want_h = next.cover_h;
    }
    rs.pending.clear();
    if (want.empty() || covers::failed(want)) {  // all there: the next neighbour on the next frame
      ++pf_k_, pf_stay_ = 0;
    } else {
      // one request, and the next only after it landed: only the newest request counts, a second one would cancel the first
      covers::request(cx.assets, cx.sv->jobs, want, want_w, want_h);
      if (++pf_stay_ >= 4) ++pf_k_, pf_stay_ = 0;  // a picture that never comes does not hold the others back
    }
    return;
  }
}

const std::string& ListScreen::row_picture(const Ctx& cx, size_t row) {
  static const std::string none;
  if (row >= rows_.size() || row >= rowimg_.size()) return none;
  if (!rowimg_have_[row]) {
    rowimg_have_[row] = 1;
    const System& sys = cx.sys_at(si_);
    if (rows_[row] < sys.games.size()) rowimg_[row] = detailed_image(details_sys(cx, sys, sys.games[rows_[row]]), sys.games[rows_[row]]);
  }
  return rowimg_[row];
}

void ListScreen::take_search(Ctx& cx) {
  sync_opts(cx);  // options changed in the menus above the list
  if (g_random_pending) {  // RANDOM GAME: another game than the cursor's
    g_random_pending = false;
    if (rows_.size() <= 1) {  // a filter or a search left one row: there is no other game, and the key says so
      cx.sv->toasts.push(tr("NO OTHER GAME"), 2500);
    } else {
      std::uniform_int_distribution<size_t> pick(0, rows_.size() - 2);
      const size_t r = pick(rng_);
      cur_ = r >= cur_ ? r + 1 : r;
      remember(cx);
      redraw_ = true;
      if (oscontract::selected_listening("game-selected")) sel_wait_ = 0;
    }
  }
  if (!box_->ready) return;
  box_->ready = false;
  const std::string text = box_->text;
  search(cx, text);
}

size_t ListScreen::letter_before() const {
  if (names_.empty()) return cur_;
  size_t start = cur_;  // first row of the letter of the cursor
  while (start > 0 && letter_key(names_[start - 1]) == letter_key(names_[cur_])) --start;
  if (start < cur_) return start;
  if (start == 0) return cur_;  // already the first letter
  const std::string prev = letter_key(names_[start - 1]);
  size_t s = start - 1;  // first row of the letter before
  while (s > 0 && letter_key(names_[s - 1]) == prev) --s;
  return s;
}

size_t ListScreen::letter_after() const {
  if (names_.empty()) return cur_;
  const std::string k = letter_key(names_[cur_]);
  for (size_t r = cur_ + 1; r < names_.size(); ++r)
    if (letter_key(names_[r]) != k) return r;
  return cur_;  // already the last letter
}

Transition ListScreen::handle(Action a, Ctx& cx) {
  sync_games(cx);
  take_search(cx);  // a search the keyboard just gave applies before this key
  size_t len = names_.size();
  const size_t prev = cur_;
  if ((a == Action::Right || a == Action::Left) && cx.es.get_bool("QuickSystemSelect", true)) {  // ES: left / right = next / previous system
    const size_t n = cx.items.empty() ? cx.sys_total() : cx.items.size();
    const long at = shown_item(cx);
    if (n < 2 || at < 0) return Transition::none();
    const size_t to = (size_t(at) + (a == Action::Right ? 1 : n - 1)) % n;
    sound::play(cx, sound::Cue::Scroll);
    save_cursor(cx);
    return Transition::replace(std::make_unique<ListScreen>(cx.system_of(to), cx));  // L / R keep paging
  }
  if (a == Action::Up || a == Action::Down || a == Action::Left || a == Action::Right || a == Action::L || a == Action::R) settle_ = 0;
  prefetch_reset();  // the neighbours are those of the cursor's new row
  idle_ready_ = false;
  switch (a) {
    case Action::B:
      if (!query_.empty()) {  // a search is on: B shows the whole list again first
        search(cx, std::string());
        return Transition::none();
      }
      sound::play(cx, sound::Cue::Back);
      save_cursor(cx);
      return Transition::pop();
    case Action::Down:
      if (len > 0) cur_ = (cur_ + 1) % len;
      break;
    case Action::Up:
      if (len > 0) cur_ = (cur_ + len - 1) % len;
      break;
    case Action::Right:
    case Action::R: {
      const size_t page = list_page_rows(lines_, shown_rows_);  // the rows on screen, not what the theme says of a layout it does not use
      if (len > 0) cur_ = std::min(cur_ + page, len - 1);
      break;
    }
    case Action::Left:
    case Action::L: {
      const size_t page = list_page_rows(lines_, shown_rows_);
      if (len > 0) cur_ = cur_ > page ? cur_ - page : 0;
      break;
    }
    case Action::A:
      if (len > 0) {
        colsys::Real r;  // a collection's game starts through the system it belongs to: its emulator, its gamelist, its play count
        if (!colsys::resolve(cx, si_, game(), r)) {
          cx.sv->toasts.push(tr("GAME NOT FOUND"), 2500);
          break;
        }
        if (Transition m = savestates::launch_request(cx, r.sys, r.game); m.kind != Transition::None) return m;  // HOOK menu-collections: savestate manager first
        Transition t = Transition::launch(command(cx));
        t.sys = int(r.sys);
        t.game = int(r.game);
        return launchcheck::guard(cx, r.sys, std::move(t));  // CheckBiosesAtLaunch
      }
      break;
    case Action::L2:  // letter jump (ES: L2 / R2): the start of the letter before / after
      if (len > 0) cur_ = letter_before();
      break;
    case Action::R2:
      if (len > 0) cur_ = letter_after();
      break;
    case Action::Start:  // the main menu, as on the carousel
      return Transition::open("menu.main");
    case Action::Select:  // ES: gamelist options; here the options of this game (menu-game)
    case Action::X:       // the help bar says X is GAME OPTIONS
      if (len > 0) {  // a collection's game is edited in the system it belongs to
        colsys::Real r;
        if (!colsys::resolve(cx, si_, game(), r)) break;
        return Transition::open("menu.game_options", cx.systems[r.sys].name + "\t" + cx.systems[r.sys].games[r.game].path);
      }
      // a filter left nothing to open the game options on: the list options are the way back
      if (listopts::filter_of(cx.sys_at(si_).name).active()) return Transition::open("menu.list_options", cx.sys_at(si_).name);
      break;
    case Action::Y: {  // quick search: the keyboard gives the text the list keeps (empty: all the games again)
      ui::KeyboardOpts o;
      o.title = tr("SEARCH");
      o.max_len = 40;
      std::shared_ptr<SearchBox> box = box_;
      return ui::keyboard(o, query_, [box](Ctx&, const std::string& text) {
        box->text = text;
        box->ready = true;
      });
    }
    case Action::R3:  // HOOK menu-collections: favourite on/off (ES: long X; here X is "game options"); ES Kid mode has no favourites
      if (len > 0 && uimode::of(cx.es) != uimode::Mode::Kid) {
        colsys::Real r;
        if (colsys::resolve(cx, si_, game(), r)) collections::toggle_favorite(cx, r.sys, r.game);  // the source's gamelist gets the flag
      }
      break;
    case Action::L3:  // HOOK menu-collections: savestate manager (ES: L3 / long A)
      if (len > 0) {
        colsys::Real r;
        if (!colsys::resolve(cx, si_, game(), r)) break;
        return savestates::open_manager(cx, r.sys, r.game);
      }
      break;
    default:
      break;
  }
  remember(cx);
  if (cur_ != prev) sound::play(cx, sound::Cue::Scroll);  // only when the cursor really moved
  if (cur_ != prev && oscontract::selected_listening("game-selected")) sel_wait_ = 0;  // (re)start the rest timer of the event
  return Transition::none();
}

std::string ListScreen::command(Ctx& cx, bool open) {
  colsys::Real r;  // the system the game really belongs to (itself, unless this list is a collection)
  if (!colsys::resolve(cx, si_, game(), r, open)) return std::string();
  const System& sys = cx.systems[r.sys];
  std::vector<JoyInfo> joys;
  if (cx.pad) joys = cx.pad->joysticks();
  return launch::build_command(sys, sys.games[r.game], cx.conf.map(), joys) + savestates::plain_args(cx, r.sys, r.game);  // HOOK menu-collections
}

std::string ListScreen::launch_target(Ctx& cx) {
  sync_games(cx);
  return cur_ < names_.size() ? command(cx, false) : std::string();  // idle time: never reads a gamelist for a collection's game
}

// No animation here, only timers: `tick` never asks the loop to spin; it marks a redraw when the settle delay ends or the
// description scrolls a pixel, and `wake_in` tells the loop when to wake up (it sleeps in between).
bool ListScreen::tick(float dt, Ctx& cx) {
  sync_games(cx);
  take_search(cx);
  if (view_ < 0.0f || !cx.ui_motion || names_.empty()) {
    view_ = float(cur_);
  } else if (view_ != float(cur_)) {  // the view slides to the cursor; from afar (a jump, the end of the list to the start) it comes from a little before it
    const float limit = float(lines_) * 2.0f;
    view_ = std::clamp(view_, float(cur_) - limit, float(cur_) + limit);
    view_ += (float(cur_) - view_) * (1.0f - std::exp(-26.0f * dt));
    if (std::fabs(float(cur_) - view_) < 0.01f) view_ = float(cur_);
    redraw_ = true;
  }
  if (sel_wait_ >= 0) {
    sel_wait_ += double(dt);
    if (sel_wait_ >= kSettleSeconds) {
      sel_wait_ = -1;
      if (!rows_.empty()) {
        const Game& g = cx.sys_at(si_).games[game()];
        oscontract::fire_game_selected(cx.sys_at(si_).name, g.path, g.name);
      }
    }
  }
  if (vpreview::take_fresh()) redraw_ = true;  // the preview's first picture: once more, without the cover under the video
  if (!detailed_enabled()) return false;
  double before = settle_;
  settle_ += double(dt);
  if (before < kSettleSeconds) {
    if (settle_ >= kSettleSeconds) redraw_ = true;  // cover and description appear
  } else if (max_scroll_ > 0 && desc_scroll_offset(before - kSettleSeconds, max_scroll_) != desc_scroll_offset(settle_ - kSettleSeconds, max_scroll_)) {
    redraw_ = true;
  } else if (genre_w_ > 0 && genre_scroll_offset(before - kSettleSeconds, genre_w_, genre_speed_) != genre_scroll_offset(settle_ - kSettleSeconds, genre_w_, genre_speed_)) {
    redraw_ = true;
  }
  // The cover and the description of the cursor are there and nothing is being decoded: a neighbour is made ready on the next frame.
  if (idle_ready_ && pf_k_ < 2 * kPrefetchRows && settle_ >= double(kSettleSeconds) + double(kPrefetchDelay) && covers::in_flight() == 0) {
    pf_run_ = true;
    redraw_ = true;
  }
  return false;
}

std::optional<float> ListScreen::wake_in() const {
  if (view_ >= 0.0f && view_ != float(cur_)) return 0.0f;
  std::optional<float> d = detail_wake_in();
  if (idle_ready_ && pf_k_ < 2 * kPrefetchRows && covers::in_flight() == 0) {  // a neighbour waits to be made ready
    const double at = double(kSettleSeconds) + double(kPrefetchDelay);
    const float s = settle_ < at ? float(at - settle_) + 0.002f : 0.0f;
    d = d ? std::min(*d, s) : s;
  }
  if (sel_wait_ < 0) return d;
  float s = std::max(float(kSettleSeconds - sel_wait_), 0.0f) + 0.002f;
  return d ? std::min(*d, s) : s;
}

std::optional<float> ListScreen::detail_wake_in() const {
  if (!detailed_enabled()) return std::nullopt;
  double s;
  if (settle_ < kSettleSeconds) {
    s = kSettleSeconds - settle_;
  } else if (max_scroll_ > 0 || genre_w_ > 0) {
    s = std::min(desc_scroll_next(settle_ - kSettleSeconds, max_scroll_), genre_scroll_next(settle_ - kSettleSeconds, genre_w_, genre_speed_));
    if (std::isinf(s)) return std::nullopt;
  } else {
    return std::nullopt;
  }
  return float(s) + 0.002f;  // a hair late rather than a hair early (no double wake-up)
}

void ListScreen::draw(Ctx& cx, std::vector<Op>& out) {
  sync_games(cx);
  bool settled = settle_ >= kSettleSeconds;
  if (rows_.empty()) {  // a search with no match: nothing to describe
    d_have_ = false;
    d_valid_ = false;
  } else if (detailed_enabled() && (!d_valid_ || d_idx_ != game() || d_settled_ != settled)) {
    {
      const System& dsys = cx.sys_at(si_);
      d_have_ = game() < dsys.games.size() && detailed_for(details_sys(cx, dsys, dsys.games[game()]), dsys.games[game()], settled, d_);
    }
    d_valid_ = true, d_idx_ = game(), d_settled_ = settled;
  }
  if (d_have_) {
    d_.desc_time = settled ? settle_ - kSettleSeconds : -1.0;
    d_.decode_inline = !cx.async_covers;
  }
  remake::DrawState& rs = remake::state();
  rs.inline_decode = !cx.async_covers;
  rs.pending.clear();
  rs.system_theme = cx.sys_at(si_).theme;
  rs.list_cam = view_ < 0.0f ? float(cur_) : view_;
  if (remake::cross(cx.th) && settled) rs.row_image = [&cx, this](size_t i) { return row_picture(cx, i); };  // asked once per row, not on every frame
  else rs.row_image = nullptr;
  const std::string title = query_.empty() ? cx.sys_at(si_).fullname : cx.sys_at(si_).fullname + ": " + query_;
  if (pf_run_) {  // first, so that what the cursor's own frame asks for is the newest request
    pf_run_ = false;
    prefetch_step(cx, title);
  }
  video::box_reset();  // the theme offers the box of the game's picture while it draws (video preview)
  rs.pending.clear();
  rs.rows_shown = 0;
  list_view(cx.th, cx.assets, cx.common, cx.w, cx.h, title, names_, cur_, cx.status, d_have_ ? &d_ : nullptr, &marks_, out);
  rs.list_cam = -1.0f;
  shown_rows_ = rs.rows_shown;
  vpreview::draw(cx, out, d_have_ ? d_.video : std::string());  // ShowVideoPreviews: the game's video over its picture, once the cursor rests
  if (!rs.pending.empty()) covers::request(cx.assets, cx.sv->jobs, rs.pending, rs.pending_w, rs.pending_h);
  max_scroll_ = d_have_ ? d_.desc_max_scroll : 0;
  genre_w_ = d_have_ ? d_.genre_w : 0;
  genre_speed_ = d_have_ ? d_.genre_speed : 0;
  // the cover is decoded on the job thread; its `done` inserts it and the loop redraws (covers.hpp)
  if (d_have_ && d_.cover_missing) covers::request(cx.assets, cx.sv->jobs, d_.image, d_.cover_w, d_.cover_h);
  // everything of this frame is there (no picture missing): the neighbours may be made ready while the cursor rests
  idle_ready_ = cx.async_covers && settled && d_have_ && !d_.cover_missing && rs.pending.empty();
}

}  // namespace zf
