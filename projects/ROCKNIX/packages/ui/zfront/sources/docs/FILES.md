# File manager (`menu.files`)

zfront's own file manager. It replaces the TOOLS entry "File Manager" (the program `fileman` / `commander` of the image): pressing A on that
entry opens this screen inside zfront instead of running an external program (`is_file_manager_script`, `menu_files.hpp`; `screens.cpp`).

## Look
One neutral look in every theme: a fixed grey-blue palette (`look()` in `menu_files.cpp`) that only follows light / dark, no skin and no backdrop. With high
contrast it uses the theme's own (high-contrast) colours. The status bar at the top and the help row at the bottom are drawn by the theme, as in every other
screen, and the text uses the theme's font. Everything is on the grid
of `remake::Grid` (cell `u`, margins of 3 cells, 12 columns, gutters of 2 cells), so it holds from 480x320 to 1280x720 and with TEXT SIZE
LARGE / EXTRA LARGE (rows and the progress card grow with the text).

From the top: the status bar (4 cells), the path pill, the list (rows of `rc` whole cells, at least 5), the free-space strip (3 cells) and the help row.
The progress card has three lines: the title, "n / total" with the percentage (or "Calculating..." while the tree is still being counted) and the name of the file in hand, cut with "...". The icon of each row is drawn by `ui::Shapes` (`Icon::Folder`, `File`, `Image`, `Game`, `Archive`, `Back`), tinted with the row's colour.

## Keys
| Key | Does |
|---|---|
| Up / Down | move the cursor (wraps) |
| L / R | one page up / down (R is in the help row; at 480x320 the bar drops "Page" and "Close", which come last) |
| Left | up one folder |
| A / Right | open a folder (the ".." row goes up); on a file, its properties |
| B | clears the selection; else up one folder; at the top of the roots, closes |
| Y | mark / unmark the file and move down (selection mode: boxes instead of icons) |
| X | actions: Copy, Move, Paste here, Rename, New folder, Delete, Properties, Select all, Clear selection, Sort by, Show hidden files. The sort and "show hidden files" are remembered (`zfront.files.sort`, `zfront.files.hidden` in system.cfg) |
| Select | go to: Games, Game card, USB drives, Screenshots (`/storage/roms/screenshots`, RetroArch's folder), Home (only what exists) |
| Start | closes every menu (also in the help row, after "Page") |

## Rules (what is asked and what is not)
- Delete asks (NO is the default) and names what will go: the one marked item (not the row under the cursor, which Y has already moved down) or "N items". Copy, move, rename and new folder do not ask.
- Inside `/storage/.config` and `/storage/.update` (system files) a delete asks a second time ("This is a system file. Deleting it can break the console.") and a rename asks once before the keyboard. Nothing is blocked: an advanced user can still do it.
- The folder of each system under `/storage/roms` and `/storage/games-external/roms` cannot be deleted or renamed (what is inside can). `Policy.keep_sub` / `Policy.warn`.
- The names sort in natural order: numbers by value ("Game 2" before "Game 10"), accented Latin letters (Latin-1 range, UTF-8) next to their base letter, no locale (`fileops::natural_compare`).
- The choice of sort and of hidden files is remembered between openings.
- A name that clashes gets " (2)" before the extension: nothing is overwritten and nothing is asked.
- Copy, move and delete run on the slow job lane with a progress card; B cancels. A cancelled copy leaves no half file (copies go to `<name>.part`).
- Links are never followed: they are listed, copied as links and deleted as links. A folder link that points out of the roots cannot be entered. Copying to a card that has no links (FAT, exFAT, `symlink()` gives EPERM) skips the link, goes on and says at the end how many were skipped; a move that had to skip links keeps the originals ("originals kept"), so no link is lost.
- Before a copy, or a move to another file system, the free space of the destination is checked (a move inside one file system is a rename and asks for nothing). On FAT32 (`statfs` type msdos/vfat) a file of 4 GB or more is refused at once with "File larger than 4 GB: this card cannot hold it".
- Rename that only changes the case works on FAT / exFAT (origin and target are the same inode: it goes straight to `rename`).
- A `<name>.part` left by a copy that froze is a regular file nobody is using (one job at a time): the next copy of that name writes over it.
- A paste that the job lane refuses (another job running) does not empty the clipboard of a move.
- An error names the item that failed (the file inside a folder, the path comes from the engine) and, with several items, how many were done: `"a.bin": Not enough free space (3 of 7 done)`. `EFBIG`, `EPERM`/`EINVAL` on create, `ENOSPC`, `EROFS`/`EACCES` have their own text.
- A folder that cannot be read (removed, drive pulled out, no permission) says "Cannot read this folder" where "This folder is empty" goes.
- Roots: `/storage` and `/media` (a link to `/var/media`, where the automount puts USB drives). Never deleted, renamed or moved: the roots, `/storage/roms`,
  `/storage/games-external`, `/storage/.config`, `/storage/.update`, and any folder that is a mount point (another file system than its parent: a USB drive or a card).
- A move across file systems is copy, then delete the source; a cancel (or an error) during the copy removes the half copy and leaves the source whole.
- Not in the kiosk / kid-mode whitelist (`uimode.cpp` only allows the listed screens).

## Not done (pending)
- Reading a big folder on a worker thread with a "Reading..." (the list is read in the drawing thread; kept simple because the keys, the tests and the cursor all assume the list is there right after a move into a folder). What was done instead: the kind of an entry comes from `readdir` (`d_type`), so folders and links are not `lstat`ed (files still are, for their size; a sort by date `lstat`s everything), and the item count of a folder is read once per list (`counts_`, cleared by `refill`), never in each frame.
- Image / text viewer, zip, search, launching games from the manager.

## Code
- `fileops.hpp/.cpp`: the engine, plain POSIX (no shell, nothing to quote), tested on a temp folder: `tests/test_fileops.cpp`.
- `menu_files.cpp`: the screen. `tests/test_files_screen.cpp`: the keys on a temp folder. `tests/test_layout.cpp`: layout of `menu.files`, with a selection,
  inside a folder and with the progress card, in every size and text size (`ZF_FILES_ROOT` points it at a fixture folder).
- `ZF_FILES_ROOT=<dir>` replaces the roots (tests, harness). The screen also takes a start folder as its argument.

## Not seen on the console
Written and measured on the PC only. Not checked on a real console: the USB mount (`/var/media/<label>` by the image's automount script and udevil.conf, read, not seen), speed on the SD card, the exact
look of the icons at 480x320.
