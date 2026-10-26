// Translations: reads the same gettext `.mo` catalogs ES ships (`<dir>/<lang>/LC_MESSAGES/emulationstation2.mo`), so the
// 35 ES languages come for free. The key is the English text as ES writes it (`tr("QUIT")`).
//
// Cost model (1 GB of RAM shared with the emulators): a catalog is `mmap`ed read-only and searched in place with a binary
// search over the sorted table of the `.mo` (no hash map, no copy of the strings); `tr()` returns pointers INTO the
// mapping and does not allocate. Pages are only faulted in for the entries actually used.
//
// Lifetime: mappings are never unmapped (a screen may still hold a `const char*` from before a language change; the
// file-backed pages are cheap). Load / switch the language from the main thread only.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zf {

class SystemConf;

namespace i18n {

/// `Plural-Forms` expression (`n != 1`, `n%10==1 && n%100!=11 ? 0 : ...`): the C subset gettext uses.
class Plural {
 public:
  /// Parses `nplurals=N; plural=EXPR;`. On failure the Germanic default (`n != 1`) is used and false is returned.
  bool parse(std::string_view plural_forms_value);
  /// Index of the plural form for `n`, always < nplurals().
  unsigned eval(unsigned long n) const;
  unsigned nplurals() const { return nplurals_; }

 private:
  struct Node {
    char op = 0;  // 'n' 'k'(constant) or an operator ('!' unary, '?' ternary, '&' '|' '=' (==) '~' (!=) '<' '>' 'l' (<=) 'g' (>=) + - * / %)
    unsigned long k = 0;
    int a = -1, b = -1, c = -1;
  };
  unsigned long run(int node, unsigned long n, int depth) const;
  std::vector<Node> nodes_;
  int root_ = -1;
  unsigned nplurals_ = 2;
};

/// One `.mo` file, memory mapped (or borrowed from memory in tests). Cheap to move; never unmaps.
class Catalog {
 public:
  /// Maps `path`. Validates the header, every table entry against the file size, the NUL terminators and that the
  /// originals are sorted (what the binary search needs). A bad file leaves the catalog empty and sets `err`.
  bool open(const std::string& path, std::string* err = nullptr);
  /// Borrow `size` bytes that must outlive the catalog (tests).
  bool open_memory(const void* data, size_t size, std::string* err = nullptr);

  bool valid() const { return base_ != nullptr; }
  size_t size() const { return count_; }                 // number of entries (including the header entry)
  size_t mapped_bytes() const { return bytes_; }
  const Plural& plural() const { return plural_; }

  /// Translation (first plural form) of `key`; `nullptr` when absent or when the translation is empty.
  /// `key` is compared as a C string: for plural entries the original is `msgid\0msgid_plural` and only `msgid` is
  /// looked at; a context is `ctx\x04msgid`.
  const char* find(const char* key) const { return find_len(key, nullptr); }
  /// Same, also returns the length of the whole translation block (all plural forms, without the last NUL).
  const char* find_len(const char* key, uint32_t* total_len) const;
  /// The `form`-th plural form of `key`, `nullptr` when the entry has fewer forms.
  const char* find_form(const char* key, unsigned form) const;

  /// Entry `i` (0 <= i < size()): original and translation blocks, for tests and tools.
  const char* original(size_t i) const;
  const char* translation(size_t i) const;

 private:
  uint32_t rd(const uint8_t* p) const;
  bool read_entry(const uint8_t* table, size_t i, uint32_t& len, uint32_t& off) const;
  bool load_header();
  const uint8_t* base_ = nullptr;
  size_t file_size_ = 0, bytes_ = 0;
  uint32_t count_ = 0, orig_off_ = 0, trans_off_ = 0;
  bool swap_ = false;
  Plural plural_;
};

struct Options {
  /// Directories searched in order for `<dir>/<lang>/LC_MESSAGES/<domain>.mo` (the first directory that has the file
  /// wins, for each domain on its own).
  std::vector<std::string> dirs;
  /// Catalogs consulted in order: zfront's own strings first, then ES's (same msgids as ES).
  std::vector<std::string> domains;
};

/// `$ZF_LOCALE_DIR` (colon separated), `/usr/share/locale` (what ES binds), `/storage/.config/locale`,
/// `/usr/config/locale`, `/usr/share/zfront/locale`; domains `zfront`, `emulationstation2`.
Options default_options();

/// Language codes with an ES catalog in `opts.dirs` (`es`, `zh_CN`...), sorted. For the language menu.
std::vector<std::string> available_languages(const Options& opts = default_options());

/// Fall-back chain of gettext for `lang`: `es_ES.UTF-8` -> `es_ES`, `es`. Empty for English (`en_US`, `en`, `C`, "").
std::vector<std::string> candidates(std::string_view lang);

/// Loads the catalogs for `system.language` and installs the translator (`set_translator`). Idempotent: nothing
/// is reloaded when the language and options did not change. Returns false when a non-English language has no catalog
/// (everything then shows English).
bool init(const SystemConf& conf, const Options& opts = default_options());
/// Same for an explicit language code (the language menu calls this after `conf.persist("system.language", ..)`).
bool set_language(std::string_view lang, const Options& opts = default_options());

/// Bumped every time a different catalog set is installed (`set_language`, also with the same code but other options):
/// a cache of translated text is valid while `generation()` has the value it was built with.
unsigned generation();
/// Language code currently loaded ("" = English / none).
const std::string& language();
/// Catalogs currently active (0 = English).
size_t loaded_catalogs();
/// Total bytes mapped by the active catalogs.
size_t mapped_bytes();
/// ES draws Arabic and Hebrew right to left.
bool is_rtl();

/// True when `s` has at least one capital letter and no lower-case one ("GAME SETTINGS", "WI-FI"; not "Wi-Fi", "%d DAYS ago", "123").
bool is_all_caps(std::string_view s);
/// Sentence case of an all-caps label: "SWITCHES AND SLIDERS" -> "Switches and sliders", "WI-FI PASSWORD" -> "Wi-Fi password",
/// "SD CARD" -> "SD card", "CONFIGURACIÓN DE RED" -> "Configuración de red". Acronyms, units and brands of a fixed list keep their
/// spelling (WI-FI IP HDMI SD USB CPU GPU RGB LED BIOS DNS SSH OTA PSP GBA NES SNES PS1 PS2 N64 NDS 3DS RAM FPS CRT LCD ES OS ID VM OK
/// MAME FBNeo RetroArch Bluetooth...; "5GHZ" -> "5GHz"). A string that is not all caps comes back unchanged.
std::string sentence_case(std::string_view s);
/// What the UI shows for a key no catalog translated: `sentence_case(key)`, kept in a table so the returned pointer is stable
/// and a repeated call allocates nothing (a string that is not all caps returns `key` itself). `tr()` uses it after the catalog
/// lookup, so a translation found in a catalog is shown as written there.
const char* display_text(const char* key);

/// Plural form: `ntr("%d day ago", "%d days ago", n)`; with no translation it picks `n != 1`.
const char* ntr(const char* singular, const char* plural, unsigned long n);
/// Context (gettext `msgctxt`): `ctr("game_options", "DELETE")`; no translation returns `text`.
const char* ctr(const char* context, const char* text);

}  // namespace i18n
}  // namespace zf
