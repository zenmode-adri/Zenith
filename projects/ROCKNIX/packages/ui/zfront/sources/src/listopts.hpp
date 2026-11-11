// MODULE `listopts`. ES's gamelist options: SORT GAMES BY (GuiGamelistOptions, `<system>.sort`), the favourites / genre filters and the
// GAMELIST OPTIONS group of the UI settings (FavoritesFirst, ShowHiddenFiles, ShowFilenames, IgnoreLeadingArticles).
//
// The game list never reorders `System::games` (launch, favourites, metadata edits and events all index it): it asks `arrange` for the
// games to show, in order. Hidden games and the article rule are decided when the games are read (`set_load_options`), so a change of
// those two makes the next list read its system again (`prepare`).
#pragma once
#include <string>
#include <vector>

#include "app.hpp"
#include "menu.hpp"
#include "settings.hpp"
#include "systems.hpp"

namespace zf::listopts {

struct Opts {
  bool favorites_first = false;   // FavoritesFirst (default off)
  bool show_filenames = false;    // ShowFilenames (default off)
  bool show_hidden = false;       // ShowHiddenFiles (default off)
  bool ignore_articles = true;    // IgnoreLeadingArticles (default on)
  int sort = 0;                   // `<system>.sort`: an ES sort id (see `sort_types`)
};

/// Session filter of one system (ES keeps its filters in memory only).
struct Filter {
  bool favorites_only = false;
  std::string genre;  // "" = any; compared without case
  bool active() const { return favorites_only || !genre.empty(); }
};

struct SortType {
  int id;            // ES's SortId
  const char* label;  // ES's text (translated by the caller)
};
/// The ES sorts zfront has data for (developer, publisher, system and file creation date are left out).
const std::vector<SortType>& sort_types();

/// The options of `system`: the global ES keys and, for FavoritesFirst and ShowFilenames, the per-system override.
Opts read(const EsSettings& es, const std::string& system);

Filter& filter_of(const std::string& system);

/// Bumped by every change made here; a list compares it with the value it saw.
unsigned generation();
void changed();

/// Name shown for a game (`ShowFilenames`: the file name without extension).
std::string display_name(const Game& g, const Opts& o);

/// Rows of the list: indices into `games`, filtered (`query_lower` = quick search, already lower case) and sorted.
/// `games` is in load order (sortname / name, articles as `Opts::ignore_articles` said when they were read).
std::vector<size_t> arrange(const std::vector<Game>& games, const Opts& o, const Filter& f, const std::string& query_lower, bool favorites_collection = false);

/// Before a list opens (and when options change under it): publishes the load options and, if this system was read with other
/// ones, reads it again.
void prepare(Ctx& cx, size_t si);

/// The four switches under the GAMELIST OPTIONS header (menu.ui and its own screen).
void build_group(MenuBuilder& m, Ctx& cx, bool header = true);

}  // namespace zf::listopts
