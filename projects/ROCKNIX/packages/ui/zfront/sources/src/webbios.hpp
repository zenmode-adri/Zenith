// MODULE `webbios`: the BIOS tab of the web file transfer. Reads the list of BIOS files the image knows (`/usr/bin/rocknix-systems`, a
// Python script whose `systems = {...}` table holds every file with its md5; ZF_ROCKNIX_SYSTEMS moves it) without running Python, says
// for each file whether it is there and right, and takes an upload for exactly one entry of that list.
//
//   GET  /files/bios[?all=1]    JSON: the systems that have games (all=1: every system of the list), each file `ok` | `wrong` (there,
//                               md5 of no variant) | `missing` | `unchecked` (a .zip: its md5 is of files inside), the accepted md5s
//   POST /files/bios/put?file=<the "file" value of the list>[&force=1][&replace=1]    body = the file, STREAMED
//
// Safety: the destination is only `<root>/<file>` with `file` taken from the list (ZF_BIOS_ROOT, default /storage/roms): a value
// that is not in the list is refused, the client never names a path. Folders are walked one component at a time with O_NOFOLLOW
// (a symlinked folder is neither read nor written through) and created inside the root only. The upload goes to `<dest>.part`, its
// md5 is computed while writing; a right md5 renames it, a wrong one deletes it (422) unless `force=1`. An existing file with a right
// md5 (or a zip) is only replaced with `replace=1`. The md5 of a file already on disk is cached by (path, size, mtime).
#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace zf {
namespace webbios {

/// One file of one system. Several entries of the script with the same `file` are variants (any of the md5 is good).
struct BiosFile {
  std::string file;                // "bios/scph5501.bin", relative to the root
  std::vector<std::string> md5s;   // lower case, accepted variants (also `altmd5`)
  bool any = false;                // an entry with md5 "": the script accepts any content
  bool zipped = false;             // the md5s are of files inside a zip (`zippedFile`): the zip itself cannot be checked
};
struct BiosSystem {
  std::string key, name;           // "psx", "Sony PlayStation"
  std::vector<BiosFile> files;     // in the script's order, one per distinct `file`
  size_t entries = 0;              // entries of the script (a zip with five files counts five)
};

/// The `systems = {...}` table of the script text. Empty when it is not there or does not parse.
std::vector<BiosSystem> parse_systems(const std::string& script);
/// How many files were hashed since the start (tests: the cache must spare the second pass).
size_t hash_count();

}  // namespace webbios
}  // namespace zf
