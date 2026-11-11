// MODULE `savebackup`: archives of the game saves (menu.savebackup). No UI here: what is saved, the archive names, the command
// lines and the create / restore / delete steps, so the menu and the tests share them.
//
// What counts as save data (paths relative to the storage root, `/storage` on the console):
//   roms/savestates/**                      savestates of every emulator
//   roms/<system>/**/*.srm *.sav *.state*   next to the ROMs (battery saves, RetroArch states)
//   roms/psp/PSP/SAVEDATA/**                PSP in-game saves
//   .config/ppsspp/PSP/SYSTEM/**            PPSSPP configuration
// ROMs, images, videos, manuals and the backup folder itself are never included.
//
// Archives live in `<storage>/roms/backup/saves-YYYYMMDD-HHMM.tar.gz` (the automatic one made before a restore adds `-pre`, `-pre2`...).
// BusyBox compatible: `tar -cf` + `-T list` and `gzip -c` as separate steps (no `-z`), extraction through a pipe, names relative to storage.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "services.hpp"

namespace zf::savebackup {

struct Plan {
  std::vector<std::string> files;  // relative to the storage root, sorted
  uint64_t bytes = 0;
};
/// Walks the save locations. Blocking (the ROM tree can be big): call it from a job.
Plan plan(const std::string& storage);

/// Whether `rel` (a path relative to the storage root, no leading '/', no '.' or '..' component) is somewhere `plan` would look: the
/// same roots and patterns, so an archive whose entries all pass holds nothing `plan` would not have put there. `is_dir`: a folder
/// entry (any folder on the way to, or inside, a save location).
bool in_plan(std::string_view rel, bool is_dir);

/// A backup / restore job is running (the menu and the web page share it: one at a time), and a counter bumped whenever the list of
/// archives may have changed.
std::atomic<bool>& busy_flag();
std::atomic<int>& generation();

/// `saves-YYYYMMDD-HHMM.tar.gz` or with `-pre` / `-preN` before the extension. The only names this module reads, restores or deletes.
bool valid_name(std::string_view name);
/// Local time as `YYYYMMDD-HHMM`.
std::string stamp_now();
/// "2026-11-09 14:30" from a valid name; "" otherwise.
std::string pretty_date(std::string_view name);
/// A valid name of the automatic backup made before a restore.
bool is_pre(std::string_view name);

struct Backup {
  std::string name;
  uint64_t bytes = 0;
};
/// The valid archives of `dir`, newest first.
std::vector<Backup> list(const std::string& dir);
/// "1.4 MB", "320 KB", "12 B".
std::string size_text(uint64_t bytes);
/// Free bytes where `dir` is (or would be: the nearest existing parent); 0 when unknown.
uint64_t free_bytes(const std::string& dir);

// ---- command lines (all paths quoted) --------------------------------------------------------------------------------------
std::string create_cmd(const std::string& storage, const std::string& dir, const std::string& name, const std::string& listfile);
std::string verify_cmd(const std::string& archive);
std::string extract_cmd(const std::string& storage, const std::string& archive);

enum class Err { None, NoSaves, NoSpace, Exists, Failed, BadName, Damaged, PreFailed };
struct Result {
  Err err = Err::None;
  std::string name;    // archive made (create) / restored (restore)
  std::string pre;     // restore: the archive of the saves that were replaced ("" when there were none)
  size_t files = 0;
  uint64_t bytes = 0;  // size of the archive made
};

/// Makes `saves-<stamp>.tar.gz` (`pre`: the first free `saves-<stamp>-pre[N].tar.gz`). Needs 2x the size of the saves free.
/// Blocking: call it from a job. `tmp` is where the file list goes.
Result create(Services& sv, const std::string& storage, const std::string& tmp, const std::string& stamp, bool pre);
/// Checks the archive, backs the current saves up (`-pre`), then extracts. Aborts without touching anything when the
/// check or the automatic backup fails.
Result restore(Services& sv, const std::string& storage, const std::string& tmp, const std::string& stamp, const std::string& name);
/// Deletes `name` from the backup folder, only when `valid_name(name)`.
bool remove_backup(const std::string& dir, const std::string& name);

inline std::string backup_dir(const std::string& storage) { return storage + "/roms/backup"; }

}  // namespace zf::savebackup
