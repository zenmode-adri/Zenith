// `menu.game.manual` / `menu.game.magazine` (arg = `game_arg(system, rom)`): the PDF viewer of ES (GuiImageViewer::showPdf), reached from the
// options of a game (GuiGameOptions: VIEW GAME MANUAL / VIEW GAME MAGAZINE, shown when pdftoppm and pdfinfo exist and the game's `manual` /
// `magazine` file does). The PDF is turned into pictures by the same external programs and options as ES (ApiSystem::getPdfPageCount /
// extractPdfImages): `pdfinfo` for the page count and `pdftoppm -jpeg -r <96|125> -cropbox -f N -l N` for one page at a time, into ES's folder
// `<user dir>/pdftmp`, emptied when the viewer closes. Pages are extracted lazily on the job thread, the page on screen first and then its
// neighbours (ES extracts them all, two at a time); the pictures are decoded on the job thread too.
//
// Keys: Left / Right turn the page (while the page fits the screen), L / R zoom out / in, A zooms in / out, the d-pad moves a zoomed page,
// B closes. ES's zoom view re-extracts the page at 300 dpi; zfront zooms the page it already has (125 dpi, 96 on small screens).
// Everything but the screen is plain functions, so the tests can check the command lines, the cache names and the parsing.
#pragma once
#include <string>
#include <vector>

#include "menu.hpp"

namespace zf::manual {

enum class Kind { Manual, Magazine };

/// `Pages:` of `pdfinfo` output (ES `getPdfPageCount`): the number after the colon of the line whose key is exactly `Pages`; 0 when absent.
int parse_page_count(const std::string& pdfinfo_out);
/// ES: `pdfinfo "<file>"`.
std::string pdfinfo_command(const std::string& pdf);
/// ES `Renderer::isSmallScreen`: the setting `ForceSmallScreen` ("true" / "false") when set, else a side of 480 pixels or less.
bool small_screen(unsigned w, unsigned h, const std::string* force);
/// ES `extractPdfImages`: 96 dpi on small screens, 125 otherwise.
int page_dpi(bool small);
/// Name every file of a page starts with (ES): `<file name of the pdf>-<dpi>-<page, 8 digits>-pdf`; pdftoppm adds its own `-<n>.jpg`.
std::string page_prefix(const std::string& pdf, int dpi, int page);
/// ES (not Windows): `pdftoppm -jpeg -r <dpi> -cropbox -f <page> -l <page> "<pdf>" "<dir>/<prefix>"`.
std::string extract_command(const std::string& pdf, int dpi, int page, const std::string& dir, const std::string& prefix);
/// The file of the page among the names of the cache folder: the first (sorted) jpg / png / ppm that starts with `prefix`; "" when none.
std::string find_page_file(std::vector<std::string> names, const std::string& prefix);

/// ES's folder of the extracted pages (`<mg::env().user_dir>/pdftmp`, re-rootable like the other paths of the game menus).
std::string cache_dir();
/// ES `isScriptingSupported(PDFEXTRACTION)`: both `pdftoppm` and `pdfinfo` are in `mg::env().bin_dir`.
bool available();
/// Absolute path of the game's manual / magazine file, "" when the gamelist has none or the file is not there.
std::string media_file(const Ctx& cx, const std::string& system, const std::string& rom_path, Kind k);

/// The GAME MEDIA rows of a game's options (ES GuiGameOptions): VIEW GAME MANUAL and VIEW GAME MAGAZINE, each only when its file exists
/// and the PDF programs are installed; no header when there is no row.
void media_entries(MenuBuilder& m, Ctx& cx, const std::string& system, const std::string& rom_path);

/// The screen, for the tests (the registry makes it too). Takes the PDF path itself.
struct Box {
  float x = 0, y = 0, w = 0, h = 0;
};
std::unique_ptr<Screen> make_viewer(Ctx& cx, const std::string& pdf);
/// Tests: what a viewer made by `make_viewer` shows.
struct ViewerState {
  int pages = 0, page = 0;
  bool closed = false;
  float zoom = 1.0f;
  bool failed = false;  // the last draw said the page could not be shown (no picture came out of pdftoppm / it would not decode)
  Box counter;  // the page counter of the last draw (zero size before the first)
  Box area;     // where the page is drawn: under the title row, above the help bar (zero size before the first draw)
};
ViewerState viewer_state(const Screen& s);  // `s` must be a viewer (make_viewer or menu.game.manual/magazine)

}  // namespace zf::manual
