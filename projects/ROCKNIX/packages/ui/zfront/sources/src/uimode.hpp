// ES's USER INTERFACE MODE (UIModeController, es_settings.cfg `UIMode`): Full, Kiosk or Kid. A console locked in ES stays locked in
// zfront because both read the same setting.
//
// What each mode takes away (ES: isUIModeFull / Kid / Kiosk in GuiMenu, GuiGameOptions, GuiGamelistOptions, FileData, SystemData):
//   Kiosk  main menu = INFORMATION, UNLOCK USER INTERFACE MODE, QUIT (no settings); no game options (ES: delete, collections, scrape,
//          edit, advanced game options); hidden games stay hidden; systems without games are not loaded (LoadEmptySystems is Full only).
//   Kid    all of Kiosk plus: only games with `<kidgame>true</kidgame>` in their gamelist (the default is false, so a game with no
//          metadata is not shown) and no favourite toggle. ES also removes the START key hint, but Start still opens the menu (so does zfront).
// The way back to Full: main menu > UNLOCK USER INTERFACE MODE, then the code of `UIMode_passkey` (default "aaaba": u d l r a b x y)
// with the pad buttons. ES leaves the window with the code only; zfront also closes it with Start.
//
// `ForceKid` / `ForceKiosk` (es_settings.cfg booleans; ES sets them from --force-kid / --force-kiosk, zfront does not read the
// command line) win over `UIMode`, Kid over Kiosk. A `UIMode` that is neither Full, Kiosk nor Kid (ES's unused "Basic") counts as Kiosk.
//
// Deviation from ES on purpose: when a restricted mode leaves no system to show ES silently sets UIMode back to Full. A locked console
// must stay locked, so zfront keeps the mode (the unlock screen is still reachable from the main menu).
#pragma once
#include <string>
#include <string_view>

#include "input.hpp"
#include "settings.hpp"

namespace zf::uimode {

enum class Mode { Full, Kiosk, Kid };

/// Mode the settings ask for.
Mode of(const EsSettings& es);
/// The mode the process works with (game lists and system loading read it, they have no `Ctx`): set by `apply`, Full until then.
void apply(const EsSettings& es);
Mode current();

/// May a screen registered as `name` be opened in `mode`? Full: all. Otherwise only the main menu, INFORMATION, QUIT, the unlock
/// screen, the savestate manager and the controller detection (a launch or a new pad needs them).
bool screen_allowed(Mode mode, std::string_view name);

/// `UIMode_passkey`, "aaaba" when not set.
std::string passkey(const EsSettings& es);
/// The code as ES prints it: "A, A, A, B, A", arrows for u d l r.
std::string passkey_text(const std::string& seq);

/// Follows the pad input the way ES's `UIModeController::listen` does: every press that is not the next key of the code starts over
/// (it is not retried as the first key); true when the whole code has been entered (and the count restarts).
class Unlock {
 public:
  explicit Unlock(std::string seq) : seq_(std::move(seq)) {}
  /// `invert`: ES `InvertButtons` (true by default): the logical A is then the es_input.cfg name "a".
  bool feed(Action a, bool invert);
  size_t progress() const { return n_; }

 private:
  std::string seq_;
  size_t n_ = 0;
};

/// Stores `mode` in es_settings.cfg (UIMode), applies it to the process, and asks for the game lists to be read again.
/// Returns false when the file could not be written.
bool set_mode(EsSettings& es, Mode mode);

}  // namespace zf::uimode
