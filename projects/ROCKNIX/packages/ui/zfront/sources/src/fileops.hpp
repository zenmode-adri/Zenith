// File operations of the file manager (`menu.files`): listing, copy, move, delete, rename, new folder, free space. No SDL, no Ctx: plain POSIX calls
// (never a shell command, so there is nothing to quote), so tests run it on a temp folder. Rules:
//  - a symbolic link is an entry of its own: it is never followed to read a size, to recurse or to delete (deleting removes the link only),
//  - a name that clashes gets " (2)", " (3)"... before the extension (no question asked),
//  - a copy goes to "<name>.part" and is renamed when whole; a cancelled one leaves no half file,
//  - the paths a `Policy` protects (the roots themselves and a few folders) cannot be deleted, renamed or moved,
//  - a symbolic link that a file system cannot hold (FAT, exFAT) is skipped and counted, never an error.
#pragma once
#include <atomic>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

namespace zf::fileops {

struct Entry {
  std::string name;
  bool dir = false;    // a real folder (a link to a folder is `link`, not `dir`; `enter` resolves it)
  bool link = false;
  bool hidden = false;  // starts with '.'
  uint64_t size = 0;   // bytes (files only)
  std::time_t mtime = 0;
};

enum class Sort { Name, Size, Date };

/// What may be touched. `roots`: nothing outside them is listed, copied into or deleted. `keep`: exact paths that are never deleted, renamed
/// or moved (the roots themselves are always kept).
struct Policy {
  std::vector<std::string> roots;
  std::vector<std::string> keep;
  std::vector<std::string> keep_sub;  // folders whose folders (and links to folders) of the first level are kept too, not what is inside them
  std::vector<std::string> warn;      // folders whose content is system files: the screen asks twice before deleting or renaming inside them
};

/// Lexical clean-up: "//" and "/./" removed, ".." resolved, no trailing slash ("/" stays). Relative paths are returned as given.
std::string normalize(const std::string& path);
/// `path` is `root` or lies under it (whole components, after `normalize`).
bool within(const std::string& path, const std::string& root);
/// The path, with every link resolved, is inside a root (a folder that is a link out of the roots is refused: nothing follows it).
bool inside_real(const Policy& p, const std::string& path);
/// Inside a root by name, and the folder it sits in really is inside one (the entry itself may be a link that points out: it can be listed
/// and removed, but not entered).
bool allowed(const Policy& p, const std::string& path);
/// Inside a root and not one of the kept paths.
bool can_change(const Policy& p, const std::string& path);
/// Inside one of `warn` (not the folder itself): a change there needs a second confirmation.
bool needs_warning(const Policy& p, const std::string& path);

/// Natural order of names: runs of digits compare as numbers ("Game 2" < "Game 10") and the common Latin letters with accents compare as their
/// base letter (UTF-8, no locale: "Ávila" next to "Avila", "ñ" with "n"). Case does not matter. Negative, zero or positive; names that only
/// differ by accent or case are then ordered by their bytes, so the order is total.
int natural_compare(const std::string& a, const std::string& b);

/// Seams for tests (a FAT card, a file system without links, a full card cannot be made in a temp folder). nullptr = the real call.
struct Hooks {
  bool (*is_fat32)(const std::string& path) = nullptr;
  int (*symlink)(const char* target, const char* link) = nullptr;
  bool (*space)(const std::string& path, uint64_t& free_bytes, uint64_t& total_bytes) = nullptr;
};
Hooks& hooks();
/// The file system of `path` is FAT12/16/32 (files of 4 GB or more do not fit).
bool is_fat32(const std::string& path);

/// Folder content, folders first, then by `sort` (name: no case; size: biggest first; date: newest first). `.` and `..` never appear.
/// Returns false when the folder cannot be read (gone, no permission, drive pulled out).
bool list(const std::string& dir, Sort sort, bool with_hidden, std::vector<Entry>& out);

/// Progress shared with the job thread: the screen reads it, the job writes it.
struct Progress {
  std::atomic<uint64_t> bytes_done{0}, bytes_total{0};
  std::atomic<int> files_done{0}, files_total{0};
  std::atomic<int> links_skipped{0};  // links a file system could not hold, left out of a copy
  std::atomic<bool> cancel{false};
  /// The file being copied or deleted now, and the one an error came from (written by the job thread, read by the screen).
  void set_current(const std::string& s) {
    std::lock_guard<std::mutex> l(mu_);
    cur_ = s;
  }
  std::string current() const {
    std::lock_guard<std::mutex> l(mu_);
    return cur_;
  }
  void set_bad(const std::string& s) {
    std::lock_guard<std::mutex> l(mu_);
    if (bad_.empty()) bad_ = s;
  }
  std::string bad() const {
    std::lock_guard<std::mutex> l(mu_);
    return bad_;
  }

 private:
  mutable std::mutex mu_;
  std::string cur_, bad_;
};

enum class Err { None, Protected, Outside, NotFound, Exists, NoSpace, Cancelled, Inside, BadName, Io, TooBig, FsName, ReadOnly };
struct Result {
  Err err = Err::None;
  int done = 0;       // items finished (a folder counts as one)
  int total = 0;      // items asked for
  int skipped = 0;    // links left out of a copy (the file system has no links)
  bool kept = false;  // a move that left the originals in place because links could not be copied
  std::string first;  // path of the last thing made (the new folder, the renamed file) or of the one that failed (the file inside a folder)
};
/// The error of an errno: EFBIG (file too big for the file system), EROFS / EACCES (read only), ENOSPC, ENOENT, EEXIST; with `creating` EPERM and
/// EINVAL mean the file system refuses that name.
Err err_from_errno(int e, bool creating);

/// " (2)" before the extension until the name is free in `dir`.
std::string unique_name(const std::string& dir, const std::string& name);
/// A name that can be given to a file: not empty, no '/', not "." or "..", at most 255 bytes.
bool valid_name(const std::string& name);

/// Sizes of what `srcs` hold (links count as 1 file of 0 bytes), for the progress bar. `p` may be null; stops early when cancelled.
void measure(const std::vector<std::string>& srcs, Progress* p);
/// Copies or moves (`move`) `srcs` into the folder `dst`. A folder cannot go into itself.
Result transfer(const Policy& pol, const std::vector<std::string>& srcs, const std::string& dst, bool move, Progress& pr);
/// Deletes `srcs` (folders with everything inside; links are removed, not followed).
Result remove(const Policy& pol, const std::vector<std::string>& srcs, Progress& pr);
Result rename_item(const Policy& pol, const std::string& path, const std::string& new_name);
Result make_dir(const Policy& pol, const std::string& parent, const std::string& name);

/// How many entries a folder holds (-1 when it cannot be read).
int count_entries(const std::string& dir, bool with_hidden);
/// Free and total bytes of the file system of `path`.
bool space(const std::string& path, uint64_t& free_bytes, uint64_t& total_bytes);
/// "4.0 MB", "212 KB", "12.4 GB" (decimal point, one decimal from MB up).
std::string size_text(uint64_t bytes);

}  // namespace zf::fileops
