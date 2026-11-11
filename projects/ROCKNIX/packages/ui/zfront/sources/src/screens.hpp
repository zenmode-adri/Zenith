// The two screens of phase 1: the system carousel and the game list.
#pragma once
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "app.hpp"

namespace zf {

class CarouselScreen : public Screen {
 public:
  explicit CarouselScreen(int start) : target_(start), cam_(float(start)) {}
  Transition handle(Action a, Ctx& cx) override;
  bool tick(float dt, Ctx& cx) override;
  void draw(Ctx& cx, std::vector<Op>& out) override;
  Profile repeat_profile() const override { return kSlow; }  // ES: the system carousel never accelerates
  /// Camera position in carousel steps (it glides towards the cursor).
  float camera() const { return cam_; }
  /// System the text under the carousel shows (-1 before the first tick) and its opacity.
  long info_item() const { return info_idx_; }
  float info_alpha() const { return info_a_; }
  /// "Continue playing" row. Down gives it the keys (when it is shown under the carousel), Left/Right pick a tile, A continues that
  /// game from its savestate, B or Up give the keys back. `row_focus` = item of `recent_items` that has them (-1: the carousel);
  /// `ring_alpha` = how strongly the focus ring shows (it fades with `cx.ui_motion`).
  int row_focus() const { return focus_; }
  float ring_alpha() const { return ring_a_; }
  const std::vector<RecentItem>& recent_items() const { return recent_.items; }
  /// Rewrites the "12 min ago" of the tiles for the time `now` (once per minute; false when no text changed). `tick` calls it.
  bool refresh_ago(const Ctx& cx, std::time_t now);
  long system_cursor(const Ctx& cx) const override;
  void reset_system_cursor(Ctx& cx, long item) override;
  /// The keys are on the carousel (not on the "continue playing" row) and the camera has arrived.
  bool snapshot_ok(const Ctx& cx) const override {
    (void)cx;
    return focus_ < 0 && std::fabs(cam_ - float(target_)) < 0.01f;
  }
  /// XMB: the drifting waves of the backdrop ask for a few frames a second; nothing else here does.
  std::optional<float> wake_in() const override { return waves_ ? std::optional<float>(wave_left_) : std::nullopt; }
  bool take_redraw() override {
    bool r = wave_redraw_;
    wave_redraw_ = false;
    return r;
  }

 private:
  bool waves_ = false, wave_redraw_ = false;
  float wave_left_ = 0.0f;
  /// The row is on screen: the carousel stopped on the system whose row it is and its text is the one shown.
  bool row_live(const Ctx& cx) const;
  /// Items of the row that are on screen right now (same geometry as the drawing), as `recent_visible`.
  std::vector<size_t> row_tiles(Ctx& cx);
  void set_focus(int item, const Ctx& cx);
  Transition launch_recent(Ctx& cx);
  int target_;  // unwrapped cursor
  float cam_;
  float room_ = -1.0f;  // Zenith Next: how much of the room of the row is open (< 0: not drawn yet)
  float room_v_ = 0.0f;  // its speed (a critically damped spring moves it)
  bool anim_ = false;
  /// The text under the carousel: system it shows (-1 = none yet) and its opacity. With `cx.ui_motion` it fades out when the cursor
  /// moves, changes system when invisible and fades back in once the camera is nearly there (ES: the system info does the same).
  long info_idx_ = -1;
  float info_a_ = 1.0f;
  /// System index the `system-selected` script event was last sent for (-1: none yet, also after coming back from a list).
  long announced_ = -1;
  /// "Continue playing" row of the system the carousel stopped on (`recent_for_`; -1 = not read yet, also after coming back from a list).
  RecentRow recent_;
  long recent_for_ = -1;
  static constexpr long kAllSystems = -2;  // Zenith Next: `recent_` holds the last games of every system
  /// Item of the row with the keys (-1 = the carousel has them). The ring is drawn on `ring_idx_` with `ring_a_` while it fades.
  int focus_ = -1, ring_idx_ = -1;
  float ring_a_ = 0.0f;
  /// Game the row had the keys on when a game was started from it: the keys go back to its tile once the row is read again.
  std::string resume_name_;
  long resume_sys_ = -1;     // and its system (two systems can have a game of the same name)
  std::time_t recent_at_ = 0;  // when the row was read or its times were last written
  void restore_focus(const Ctx& cx);
  bool reread_ = false;  // a launch left from the row: read it again on the next tick
  /// X: a random system (another one than the cursor's).
  std::mt19937 rng_{std::random_device{}()};
};

class ListScreen : public Screen {
 public:
  ListScreen(size_t system_index, Ctx& cx);
  Transition handle(Action a, Ctx& cx) override;
  bool tick(float dt, Ctx& cx) override;
  std::optional<float> wake_in() const override;
  bool take_redraw() override {
    bool r = redraw_;
    redraw_ = false;
    return r;
  }
  void draw(Ctx& cx, std::vector<Op>& out) override;
  std::string launch_target(Ctx& cx) override;
  /// Quick search (Y): the list shows the games whose name contains `text` (no case); "" shows them all again.
  void search(Ctx& cx, const std::string& text);
  long shown_item(const Ctx& cx) const override { return cx.items.empty() ? long(si_) : cx.item_of(si_); }
  /// Path of the game under the cursor ("" in an empty list).
  const std::string& cursor_path() const { return cur_path_; }

 private:
  /// The launch command of the cursor's game, through the system it really belongs to (a collection's game: its source). `open = false`
  /// leaves it empty when that system's gamelist is not read yet.
  std::string command(Ctx& cx, bool open = true);
  /// Game index of the cursor (`rows_` holds the game of each row). Needs a non-empty list.
  size_t game() const { return rows_[cur_]; }
  /// Rebuilds `rows_`, `names_` and `marks_` from the system's games and `query_`.
  void refill(Ctx& cx);
  /// The gamelist options (sort, filters, favourites on top, hidden, file names, articles) changed since the last refill: read again and
  /// show the list from its first row.
  void sync_opts(Ctx& cx);
  unsigned opts_gen_ = 0;
  /// The games were edited, scraped, hidden or deleted behind the list (`games_generation`): rebuilds the rows and keeps the cursor on the
  /// same game (by path), or on the nearest row when it is gone. Called first by every entry point, so no stale row index is ever used.
  void sync_games(Ctx& cx);
  /// Remembers the path of the game under the cursor for `sync_games`.
  void remember(const Ctx& cx);
  /// Applies what the keyboard of Y gave (its callback only stores it: the keyboard is above the list) and a RANDOM GAME request.
  void take_search(Ctx& cx);
  /// Rows the cursor jumps to with L2 (start of the letter before) and R2 (start of the letter after); no wrap.
  size_t letter_before() const;
  size_t letter_after() const;
  std::optional<float> detail_wake_in() const;
  /// The cursor of this system's list is kept when it is left (B, or L / R to another system) and put back when it opens again (ES does):
  /// by the path of the game, so a list that changed in between falls back to its first row.
  void save_cursor(const Ctx& cx) const;
  void restore_cursor(const Ctx& cx);
  /// Picture of a row (from the gamelist), asked once per row: the view of the rows with pictures asks for 6 to 19 of them on every frame.
  const std::string& row_picture(const Ctx& cx, size_t row);
  /// The neighbours of the cursor are made ready while it rests (the carousel does the same with its next logos): one per frame, from
  /// `draw`, once the cursor's own cover is there. The cover decodes on the job thread; what has to run here (the description's text, the
  /// name of its picture) runs now, in frames nobody is waiting for, not on the frame a key brings the neighbour to the cursor.
  void prefetch_step(Ctx& cx, const std::string& title);
  void prefetch_reset() { pf_k_ = pf_stay_ = 0, pf_run_ = false; }
  std::vector<std::string> rowimg_;
  std::vector<char> rowimg_have_;
  size_t shown_rows_ = 0;  // rows the last view drew (0: not known yet)
  size_t pf_k_ = 0, pf_stay_ = 0;
  bool pf_run_ = false, idle_ready_ = false;
  size_t si_, cur_ = 0, lines_;
  float view_ = -1.0f;  // the row the view is on, on its way to `cur_`
  std::vector<std::string> names_;
  /// Per game: "12 min ago" when played in the last 30 days, else empty (computed when the list opens).
  std::vector<std::string> marks_;
  /// Seconds since the cursor last moved; cover and description are skipped until it reaches `kSettleSeconds`, and
  /// the description scrolls with the time after that (`desc_scroll_offset`).
  double settle_;
  /// Right column of the selected game, rebuilt only when the game or the settled state changes.
  DetailedData d_;
  size_t d_idx_ = 0;
  bool d_valid_ = false, d_have_ = false, d_settled_ = false;
  /// How far the description can scroll (from the last draw); 0 = no timer needed.
  int max_scroll_ = 0;
  /// Width and speed of a genre that does not fit (from the last draw); 0 = no timer needed.
  float genre_w_ = 0, genre_speed_ = 0;
  bool redraw_ = false;
  /// `game-selected` script event: seconds since the cursor last moved while one is pending (-1: none). Armed only when a script
  /// listens (`oscontract::selected_listening`), fired by `tick` once the cursor rested for `kSettleSeconds`, so holding a
  /// direction sends one event, at the end.
  double sel_wait_ = -1;
  /// Quick search: the text in use ("" = none), the game of each row and the keyboard's hand-over box.
  std::string query_;
  std::vector<size_t> rows_;
  unsigned gen_ = 0;
  std::string cur_path_;
  struct SearchBox {
    bool ready = false;
    std::string text;
  };
  std::shared_ptr<SearchBox> box_ = std::make_shared<SearchBox>();
  /// Random game (Start): another game than the cursor's one.
  std::mt19937 rng_{std::random_device{}()};
};

/// Is the ROM `stem` of `sys` in its folder? One stat per extension and, if those miss, one pass over the folder comparing names without case
/// ("Game.GB" for ".gb"). No gamelist is read: false means a sub folder or a ROM that is gone.
bool rom_in_folder(const System& sys, const std::string& stem);
/// Rows an L / R press moves: those the view drew (`shown`, 0 = not known yet) or else what the theme says (`lines`).
size_t list_page_rows(size_t lines, size_t shown);

/// Random game (ES: a long press of Y, which zfront does not have): the game options offer it. The list under them moves
/// its cursor to another game the next time it runs.
void request_random_game();

/// Carousel glide: the camera closes `kGlideRate` per second of its distance to the target (first-order, frame-rate independent).
/// It answers at full speed the instant a key is pressed and has a soft tail, 90 % of a step in ~145 ms and 99 % in ~290 ms; a new key
/// press while moving only changes the target, so the speed never restarts from zero or from the peak (ES: 350 ms easeOutQuint).
constexpr float kGlideRate = 16.0f;
/// Text under the carousel: fade-out of the old system (s), fade-in of the new one (s), and how close (carousel steps) the camera
/// must be for the new text to start coming in.
constexpr float kInfoOut = 0.07f, kInfoIn = 0.14f, kInfoInAt = 0.35f;
/// Focus ring of the "continue playing" row: fade in / out (s).
constexpr float kRingFade = 0.10f;
/// Distance (in carousel steps) under which the camera is snapped onto the target and the animation ends (about 1 px).
constexpr float kGlideEnd = 0.003f;
/// A press of A is accepted once the camera is this close to the target (about 7 px): the tail of the glide must not eat the key.
constexpr float kGlideAcceptA = 0.03f;
/// Cursor rest time before the heavy parts of the right column (cover decode, description wrap) are drawn.
constexpr float kSettleSeconds = 0.12f;
/// The neighbours of the cursor start getting ready this long after the cover and the description appeared, `kPrefetchRows` on each side.
constexpr float kPrefetchDelay = 0.15f;
constexpr size_t kPrefetchRows = 2;

}  // namespace zf
