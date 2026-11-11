// MODULE `webmanage`: manage the games from the phone: trash, rename, change the picture (a web module, see webcore.hpp).
//
//   GET  /files/game/info?id=            {"file":<ROM file name>,"title":<name shown>} (what the rename form starts from)
//   POST /files/game/trash?id=           moves the ROM (and the media the gamelist names inside the ROM folder) to <rom dir>/.trash/<item>/,
//                                        takes the game out of the list and its gamelist.xml, and keeps the entry in <item>/entry.json
//   GET  /files/trash                    {"items":[{id,system,name,file,size,date}]} of every system, newest first
//   POST /files/trash/restore?item=      puts the file and the gamelist entry back (409 if a file with that name exists now)
//   POST /files/trash/empty?confirm=1    deletes the trash for good (400 without confirm=1)
//   POST /files/game/rename?id=&name=[&title=]   new ROM file name (same extension, nothing overwritten); the gamelist <path>, the saves
//                                        next to the ROM, the save states and the media named after the ROM follow
//   POST /files/game/picture?id=         STREAMED png / jpg / webp up to 8 MB (magic bytes checked) -> <rom dir>/images/<stem>-image.<ext>
//                                        (where the scraper puts it), <image> and <thumbnail> of the gamelist point to it
// The trash is inside the ROM folder tree, so a move is a rename() on one file system (a different one, e.g. a mount on .trash, is
// refused with a clear error before anything moves); the scan and the gamelist reader ignore dot folders, so .trash never shows as
// games. Saves and save states stay where they are when a game is trashed (a restore finds them again); custom collections are
// remembered in the entry. Item ids are "<system>|<folder>" with the folder name checked against a strict pattern: a client never
// names a path. Files are opened without following links and every path is checked to lie inside the ROM folder.
// The file moves run on the HTTP thread (a rename is instant, and the answer can report an error); everything that touches the
// gamelist or the games in memory goes through `Env::on_main` and the answer is 202. A game running (any) refuses trash and rename.
//   POST /files/game/find?id=            FIND PICTURE: the console opens its one-game scrape (menu.scraper.game.media: pictures only,
//                                        name and description kept) with its own progress and errors; 202, the page looks again later
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "app.hpp"
#include "collections.hpp"
#include "gamelist_game.hpp"
#include "gamemeta.hpp"
#include "i18n.hpp"
#include "json.hpp"
#include "menu_game.hpp"
#include "menu_startup.hpp"
#include "oscontract.hpp"
#include "registry.hpp"
#include "services.hpp"
#include "webcore.hpp"

namespace zf {
namespace webtransfer {
namespace {

constexpr const char* kTrashDir = ".trash";
constexpr uint64_t kMaxPicture = 8u << 20;
std::mutex g_mutex;  // one manage request at a time (file moves + their bookkeeping)

// every media field of ES's metadata (as menu delete); the plain text fields kept in the trash entry
const char* const kMedia[] = {"image", "thumbnail", "video", "marquee", "fanart", "titleshot", "manual", "magazine", "map", "cartridge", "boxart", "boxback", "wheel", "mix"};
const char* const kText[] = {"name", "desc", "genre", "players", "developer", "publisher", "releasedate", "rating", "favorite", "hidden", "kidgame",
                             "sortname", "playcount", "lastplayed", "gametime", "region", "lang", "emulator", "core"};
// RetroArch / emulator saves next to the ROM, named after its stem
const char* const kSaveExts[] = {"srm", "sav", "rtc", "mcr", "eep", "fla", "sra", "mpk", "nv"};

template <size_t N>
bool listed(const char* const (&l)[N], const std::string& s) {
  for (const char* x : l)
    if (s == x) return true;
  return false;
}

// ---- paths -------------------------------------------------------------------------------------------------------------
std::string base_of(const std::string& p) {
  size_t i = p.rfind('/');
  return i == std::string::npos ? p : p.substr(i + 1);
}
std::string dir_of(const std::string& p) {
  size_t i = p.rfind('/');
  return i == std::string::npos ? std::string() : p.substr(0, i);
}
std::string stem_of(const std::string& file) {
  size_t d = file.rfind('.');
  return d == std::string::npos || d == 0 ? file : file.substr(0, d);
}
std::string ext_of(const std::string& file) {
  size_t d = file.rfind('.');
  return d == std::string::npos || d == 0 ? std::string() : lower(file.substr(d));
}
std::string trim_slash(std::string d) {
  while (d.size() > 1 && d.back() == '/') d.pop_back();
  return d;
}
/// `path` below `root` as a relative path ("" when it is not below it).
std::string rel_under(const std::string& root, const std::string& path) {
  const std::string pre = root + "/";
  return path.size() > pre.size() && path.compare(0, pre.size(), pre) == 0 ? path.substr(pre.size()) : std::string();
}
/// A relative path of plain components (no "..", no hidden names, no control characters), at most 8 deep.
bool plain_rel(const std::string& rel) {
  if (rel.empty()) return false;
  size_t i = 0, n = 0;
  while (i <= rel.size()) {
    size_t j = rel.find('/', i);
    if (j == std::string::npos) j = rel.size();
    if (!valid_name(rel.substr(i, j - i)) || ++n > 8) return false;
    i = j + 1;
  }
  return true;
}
std::string real_of(const std::string& p) {
  char buf[4096];
  return ::realpath(p.c_str(), buf) ? std::string(buf) : std::string();
}
/// The folder of `path` resolves (links included) to `root` or below it.
bool inside_real(const std::string& root, const std::string& path) {
  const std::string rr = real_of(root), pr = real_of(dir_of(path));
  return !rr.empty() && !pr.empty() && rr != "/" && (pr == rr || pr.compare(0, rr.size() + 1, rr + "/") == 0);
}
bool is_dir_nolink(const std::string& p) {
  struct stat sb;
  return ::lstat(p.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode);
}
bool is_file_nolink(const std::string& p, uint64_t* size = nullptr) {
  struct stat sb;
  if (::lstat(p.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode)) return false;
  if (size) *size = uint64_t(sb.st_size);
  return true;
}
/// Creates the folders of `rel_dir` (relative, plain) below `root`.
bool make_dirs(const std::string& root, const std::string& rel_dir) {
  std::string cur = root;
  size_t i = 0;
  while (i < rel_dir.size()) {
    size_t j = rel_dir.find('/', i);
    if (j == std::string::npos) j = rel_dir.size();
    cur += "/" + rel_dir.substr(i, j - i);
    if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    if (!is_dir_nolink(cur)) return false;
    i = j + 1;
  }
  return true;
}
/// rename() that never replaces an existing file (renameat2 NOREPLACE; where the file system lacks it, a check first). 0 or errno.
int rename_noreplace(const std::string& from, const std::string& to) {
#ifdef SYS_renameat2
  if (::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), 1u) == 0) return 0;
  if (errno != ENOSYS && errno != EINVAL) return errno;
#endif
  struct stat sb;
  if (::lstat(to.c_str(), &sb) == 0) return EEXIST;
  return ::rename(from.c_str(), to.c_str()) == 0 ? 0 : errno;
}
bool write_text(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (fd < 0) return false;
  size_t off = 0;
  bool ok = true;
  while (ok && off < text.size()) {
    ssize_t w = ::write(fd, text.data() + off, text.size() - off);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) ok = false;
    else off += size_t(w);
  }
  ok = ok && ::fsync(fd) == 0;
  ok = (::close(fd) == 0) && ok;
  if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}
bool read_text(const std::string& path, std::string& out) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  out.clear();
  char b[4096];
  for (;;) {
    ssize_t n = ::read(fd, b, sizeof b);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    out.append(b, size_t(n));
    if (out.size() > (1u << 20)) break;
  }
  ::close(fd);
  return out.size() <= (1u << 20);
}
/// Removes a folder tree without following links.
void remove_tree(const std::string& p, int depth = 0) {
  struct stat sb;
  if (::lstat(p.c_str(), &sb) != 0) return;
  if (!S_ISDIR(sb.st_mode)) {
    ::unlink(p.c_str());
    return;
  }
  if (depth > 8) return;
  if (DIR* d = ::opendir(p.c_str())) {
    while (dirent* e = ::readdir(d)) {
      std::string n = e->d_name;
      if (n != "." && n != "..") remove_tree(p + "/" + n, depth + 1);
    }
    ::closedir(d);
  }
  ::rmdir(p.c_str());
}
std::string quoted(const std::string& s) { return "\"" + oscontract::json_escape(s) + "\""; }

// ---- the trash entry ---------------------------------------------------------------------------------------------------
struct Media {
  std::string rel, stored;  // where it lay (relative to the ROM folder), and its name in the item folder ("m0")
};
struct Entry {
  std::string system, rel, file, title;
  long long trashed = 0;
  std::vector<std::pair<std::string, std::string>> fields;  // the gamelist children as they were (text tags and media tags)
  std::vector<Media> media;
  std::vector<std::string> colls;  // custom collections the game was in
};

std::string entry_json(const Entry& e) {
  std::string s = "{\"v\":1,\"system\":" + quoted(e.system) + ",\"rel\":" + quoted(e.rel) + ",\"file\":" + quoted(e.file) + ",\"title\":" + quoted(e.title) +
                  ",\"trashed\":" + std::to_string(e.trashed) + ",\"fields\":{";
  for (size_t i = 0; i < e.fields.size(); ++i) s += (i ? "," : "") + quoted(e.fields[i].first) + ":" + quoted(e.fields[i].second);
  s += "},\"media\":[";
  for (size_t i = 0; i < e.media.size(); ++i) s += (i ? "," : "") + std::string("{\"rel\":") + quoted(e.media[i].rel) + ",\"stored\":" + quoted(e.media[i].stored) + "}";
  s += "],\"collections\":[";
  for (size_t i = 0; i < e.colls.size(); ++i) s += (i ? "," : "") + quoted(e.colls[i]);
  return s + "]}";
}

bool stored_name_ok(const std::string& s) {
  if (s.size() < 2 || s[0] != 'm' || s.size() > 6) return false;
  return std::all_of(s.begin() + 1, s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

/// Reads an entry back; everything in it that becomes a path is checked here (the file is ours, but it lies on the SD card).
bool entry_parse(const std::string& text, Entry& e) {
  json::Value v;
  if (!json::parse(text, v) || !v.is_object()) return false;
  e = Entry();
  e.system = v.text("system");
  e.rel = v.text("rel");
  e.file = v.text("file");
  e.title = v.text("title");
  e.trashed = v.integer("trashed", 0);
  if (e.system.empty() || !plain_rel(e.rel) || e.file != base_of(e.rel)) return false;
  if (const json::Value* f = v.find("fields"); f && f->is_object())
    for (const auto& kv : f->obj)
      if ((listed(kText, kv.first) || listed(kMedia, kv.first)) && kv.second.type == json::Value::Type::String) e.fields.emplace_back(kv.first, kv.second.str);
  if (const json::Value* m = v.find("media"); m && m->is_array())
    for (const json::Value& it : m->arr) {
      Media x;
      x.rel = it.text("rel");
      x.stored = it.text("stored");
      if (!plain_rel(x.rel) || !stored_name_ok(x.stored)) return false;
      e.media.push_back(x);
    }
  if (const json::Value* c = v.find("collections"); c && c->is_array())
    for (const json::Value& it : c->arr)
      if (it.type == json::Value::Type::String && !it.str.empty()) e.colls.push_back(it.str);
  return true;
}

/// YYYYMMDD-HHMMSS-xxxx (xxxx: hex), the name of an item folder.
bool item_name_ok(const std::string& n) {
  if (n.size() != 20 || n[8] != '-' || n[15] != '-') return false;
  for (size_t i = 0; i < n.size(); ++i) {
    if (i == 8 || i == 15) continue;
    const char c = n[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return true;
}

std::string trash_of(const std::string& root) { return root + "/" + kTrashDir; }

/// Puts the files of an item back. 0, EEXIST (the ROM's place is taken: nothing moved), or another errno. A medium whose place is taken
/// stays as it is on disk and its copy in the trash goes.
int put_back(const std::string& item, const std::string& root, const Entry& e) {
  if (!make_dirs(root, dir_of(e.rel))) return EIO;
  if (int rc = rename_noreplace(item + "/" + e.file, root + "/" + e.rel)) return rc;
  for (const Media& m : e.media) {
    if (!make_dirs(root, dir_of(m.rel))) continue;
    if (rename_noreplace(item + "/" + m.stored, root + "/" + m.rel) == EEXIST) ::unlink((item + "/" + m.stored).c_str());
  }
  return 0;
}
void drop_item(const std::string& item) {
  ::unlink((item + "/entry.json").c_str());
  ::unlink((item + "/entry.json.tmp").c_str());
  ::rmdir(item.c_str());  // leftovers (a medium that could not move) keep the folder; nothing lists it any more
}

std::string join_root(const std::string& rom_dir) { return trim_slash(rom_dir); }

long system_index(const Ctx& cx, const std::string& name) {
  for (size_t i = 0; i < cx.systems.size(); ++i)
    if (cx.systems[i].name == name) return long(i);
  return -1;
}

// ---- a gamelist / memory edit on the main thread -----------------------------------------------------------------------
struct Edit {
  std::string sys, rom_key;  // rom_key: the ROM path the gamelist is asked about
  std::string old_path, new_path, title;  // new_path / title: also change the game in memory
  std::vector<std::pair<std::string, std::string>> fields;
};

void run_edit(const Env& env, Edit ed) {
  std::function<void()> refresh = env.refresh;
  env.on_main([ed, refresh](App& app) {
    Ctx& cx = app.cx;
    const long si = system_index(cx, ed.sys);
    if (si < 0) return;
    struct Out {
      bool ok = true, colls = false;
      std::string err;
      GamelistEdit edit;
    };
    auto res = std::make_shared<Out>();
    const std::string xml = cx.systems[size_t(si)].gamelist, dir = cx.systems[size_t(si)].rom_dir;
    Ctx* c = &cx;
    cx.sv->jobs.post(
        [=] {
          if (!ed.fields.empty()) res->ok = gamelist_set_fields(xml, dir, ed.rom_key, ed.fields, res->err, &res->edit);
          if (!ed.new_path.empty() && ed.new_path != ed.old_path)
            for (const collections::Custom& cu : collections::list_custom()) {
              if (cu.dynamic || !collections::contains(cu.name, ed.old_path)) continue;
              std::string e;
              bool ch = false;
              if (collections::remove_game(cu.name, ed.old_path, e, &ch) && collections::add_game(cu.name, ed.new_path, e, &ch)) res->colls = true;
            }
        },
        [=] {
          if (size_t(si) >= c->systems.size()) return;
          System& s = c->systems[size_t(si)];
          if (res->ok) s.apply_edit(res->edit);
          const std::string& now = ed.new_path.empty() ? ed.old_path : ed.new_path;  // the file moved whatever the gamelist said
          for (Game& g : s.games) {
            if (g.path != ed.old_path) continue;
            g.path = now;
            if (res->ok && !ed.title.empty()) g.name = ed.title;
            if (res->ok && g.meta.span_end == 0 && res->edit.elem_end > res->edit.elem_begin && res->edit.elem_end <= 0xffffffffull) {  // an entry was made
              g.meta.span_begin = uint32_t(res->edit.elem_begin);
              g.meta.span_end = uint32_t(res->edit.elem_end);
            }
            break;
          }
          if (res->colls) collections::set_reload_needed(true);
          games_changed();
          if (!res->ok) {
            std::fprintf(stderr, "zfront: web manage: game data not saved for %s: %s\n", now.c_str(), res->err.c_str());
            c->sv->toasts.push(tr("COULD NOT SAVE GAME DATA"), 4000);
          }
          if (refresh) refresh();
        });
  });
}

// ---- routes ------------------------------------------------------------------------------------------------------------
bool param(const http::Request& req, const char* key, std::string& out) { return http::query_param(req, key, out); }

/// The game of `?id=` with everything a manage route needs checked once. `deny` holds the answer when false.
struct Target {
  FoundGame f;
  std::string root, rel;
};
bool target_of(const http::Request& req, const Env& env, Target& t, http::Response& deny, bool idle_only) {
  std::string id;
  param(req, "id", id);
  if (!find_game(id, t.f)) {
    deny = err(404, "unknown game");
    return false;
  }
  if (idle_only && oscontract::game_running()) {
    deny = err(409, "a game is running");
    return false;
  }
  if (!env.on_main) {
    deny = err(500, "game management is not available");
    return false;
  }
  t.root = join_root(t.f.rom_dir);
  t.rel = rel_under(t.root, t.f.path);
  if (t.root.empty() || !plain_rel(t.rel) || !inside_real(t.root, t.f.path)) {
    deny = err(400, "this game cannot be changed from here");
    return false;
  }
  if (!is_file_nolink(t.f.path)) {
    deny = err(404, "the file is not there");
    return false;
  }
  return true;
}

http::Response info_route(const http::Request& req, const Env& env) {
  Target t;
  http::Response deny;
  if (!target_of(req, env, t, deny, false)) return deny;
  return reply(200, "{\"file\":" + quoted(base_of(t.f.path)) + ",\"title\":" + quoted(t.f.name) + "}");
}

std::atomic<unsigned> g_item_seq{0};

http::Response trash_route(const http::Request& req, const Env& env) {
  Target t;
  http::Response deny;
  if (!target_of(req, env, t, deny, true)) return deny;
  std::lock_guard<std::mutex> lk(g_mutex);
  const FoundGame& f = t.f;

  Entry e;
  e.system = f.system;
  e.rel = t.rel;
  e.file = base_of(t.rel);
  e.title = f.name;
  e.trashed = static_cast<long long>(::time(nullptr));
  std::vector<std::string> tags(std::begin(kText), std::end(kText));
  tags.insert(tags.end(), std::begin(kMedia), std::end(kMedia));
  const auto got = gamelist_read_fields(f.gamelist, f.rom_dir, f.path, f.meta, tags);
  for (const auto& kv : got)
    if (!kv.second.empty()) e.fields.emplace_back(kv.first, kv.second);
  for (const collections::Custom& cu : collections::list_custom())
    if (!cu.dynamic && collections::contains(cu.name, f.path)) e.colls.push_back(cu.name);
  std::vector<std::string> media_from;  // absolute paths, same order as e.media
  for (const auto& kv : e.fields) {
    if (!listed(kMedia, kv.first)) continue;
    const std::string abs = resolve_rel_path(kv.second, f.rom_dir);
    const std::string mrel = rel_under(t.root, abs);
    if (abs == f.path || !plain_rel(mrel) || !inside_real(t.root, abs) || !is_file_nolink(abs)) continue;  // outside, a folder or not there: left alone
    if (std::find(media_from.begin(), media_from.end(), abs) != media_from.end()) continue;
    media_from.push_back(abs);
    e.media.push_back({mrel, "m" + std::to_string(e.media.size())});
  }

  // the item folder
  const std::string tdir = trash_of(t.root);
  if (::mkdir(tdir.c_str(), 0755) != 0 && errno != EEXIST) return err(500, "cannot create the trash folder");
  if (!is_dir_nolink(tdir)) return err(500, "the trash folder is not a folder");
  char stamp[24];
  struct tm tmv {};
  const time_t now = ::time(nullptr);
  ::localtime_r(&now, &tmv);
  std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
  std::string item;
  for (int tries = 0; tries < 64; ++tries) {
    char suffix[8];
    std::snprintf(suffix, sizeof suffix, "%04x", (g_item_seq++ + unsigned(::getpid())) & 0xffffu);
    item = tdir + "/" + stamp + "-" + suffix;
    if (::mkdir(item.c_str(), 0755) == 0) break;
    item.clear();
    if (errno != EEXIST) break;
  }
  if (item.empty()) return err(500, "cannot create the trash folder");

  std::vector<std::pair<std::string, std::string>> moved;  // (from, to)
  auto rollback = [&] {
    for (auto it = moved.rbegin(); it != moved.rend(); ++it) ::rename(it->second.c_str(), it->first.c_str());
    drop_item(item);
  };
  auto fail = [&](int rc) {
    rollback();
    if (rc == EXDEV) return err(500, "the trash is on another file system than the game; nothing was moved");
    return err(500, "could not move the file to the trash");
  };
  if (int rc = rename_noreplace(f.path, item + "/" + e.file)) return fail(rc);
  moved.emplace_back(f.path, item + "/" + e.file);
  for (size_t i = 0; i < e.media.size(); ++i) {
    const std::string to = item + "/" + e.media[i].stored;
    if (int rc = rename_noreplace(media_from[i], to)) return fail(rc);
    moved.emplace_back(media_from[i], to);
  }
  if (!write_text(item + "/entry.json", entry_json(e))) return fail(EIO);

  // the game leaves the list and the gamelist (main thread); a refusal there puts everything back
  const std::string sys = f.system, path = f.path, root = t.root;
  std::function<void()> refresh = env.refresh;
  env.on_main([sys, path, root, item, e, refresh](App& app) {
    Ctx& cx = app.cx;
    const long si = system_index(cx, sys);
    std::string why;
    bool ok = si >= 0;
    if (ok) {
      if (mg::is_startup_game(cx, path)) mg::clear_startup_game(cx);
      ok = game_delete(cx, size_t(si), path, why) == DeleteResult::Ok;
    }
    if (!ok) {
      put_back(item, root, e);
      drop_item(item);
      cx.sv->toasts.push(tr("THE GAME COULD NOT BE DELETED"), 4000);
    }
    if (refresh) refresh();
  });
  return reply(202, "{\"ok\":true}");
}

struct Item {
  std::string id, system, name, file, date;
  uint64_t size = 0;
  long long trashed = 0;
};

/// Calls `f(system, root, item folder name, entry)` for every readable item of every system, once per ROM folder.
template <class F>
void each_item(F f) {
  std::set<std::string> seen;
  for (const SystemInfo& si : systems_snapshot()) {
    const std::string root = join_root(si.rom_dir);
    if (root.empty() || !seen.insert(root).second) continue;
    DIR* d = ::opendir(trash_of(root).c_str());
    if (!d) continue;
    std::vector<std::string> names;
    while (dirent* de = ::readdir(d)) {
      std::string n = de->d_name;
      if (item_name_ok(n)) names.push_back(n);
    }
    ::closedir(d);
    for (const std::string& n : names) {
      std::string text;
      Entry e;
      if (!read_text(trash_of(root) + "/" + n + "/entry.json", text) || !entry_parse(text, e)) continue;
      f(si, root, n, e);
    }
  }
}

http::Response list_route(const http::Request&) {
  std::vector<Item> items;
  each_item([&](const SystemInfo& si, const std::string& root, const std::string& n, const Entry& e) {
    Item it;
    it.id = si.name + "|" + n;
    it.system = si.name;
    it.name = e.title.empty() ? e.file : e.title;
    it.file = e.file;
    it.trashed = e.trashed;
    is_file_nolink(trash_of(root) + "/" + n + "/" + e.file, &it.size);
    char b[24];
    struct tm tmv {};
    const time_t t = static_cast<time_t>(e.trashed);
    ::localtime_r(&t, &tmv);
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tmv);
    it.date = b;
    if (items.size() < 500) items.push_back(it);
  });
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.trashed > b.trashed || (a.trashed == b.trashed && a.id < b.id); });
  std::string s = "{\"items\":[";
  for (size_t i = 0; i < items.size(); ++i) {
    const Item& it = items[i];
    s += (i ? "," : "") + std::string("{\"id\":") + quoted(it.id) + ",\"system\":" + quoted(it.system) + ",\"name\":" + quoted(it.name) + ",\"file\":" +
         quoted(it.file) + ",\"size\":" + std::to_string(it.size) + ",\"date\":" + quoted(it.date) + "}";
  }
  return reply(200, s + "]}");
}

http::Response restore_route(const http::Request& req, const Env& env) {
  std::string id;
  param(req, "item", id);
  const size_t bar = id.find('|');
  if (bar == std::string::npos) return err(404, "unknown item");
  const std::string sysname = id.substr(0, bar), name = id.substr(bar + 1);
  SystemInfo si;
  if (!item_name_ok(name) || !find_system(sysname, si) || si.rom_dir.empty()) return err(404, "unknown item");
  if (!env.on_main) return err(500, "game management is not available");
  std::lock_guard<std::mutex> lk(g_mutex);
  const std::string root = join_root(si.rom_dir), item = trash_of(root) + "/" + name;
  std::string text;
  Entry e;
  if (!read_text(item + "/entry.json", text) || !entry_parse(text, e) || e.system != sysname) return err(404, "unknown item");
  if (!is_file_nolink(item + "/" + e.file)) return err(404, "the file is not in the trash any more");
  if (int rc = put_back(item, root, e)) return rc == EEXIST ? err(409, "a file with that name exists now") : err(500, "could not put the file back");

  std::function<void()> refresh = env.refresh, reload = env.reload;
  env.on_main([e, root, item, refresh, reload](App& app) {
    Ctx& cx = app.cx;
    const long si = system_index(cx, e.system);
    if (si < 0) {
      drop_item(item);
      if (reload) reload();
      return;
    }
    struct Out {
      bool ok = true;
      std::string err;
    };
    auto res = std::make_shared<Out>();
    const std::string xml = cx.systems[size_t(si)].gamelist, dir = cx.systems[size_t(si)].rom_dir, path = root + "/" + e.rel;
    Ctx* c = &cx;
    cx.sv->jobs.post(
        [=] {
          if (!e.fields.empty()) res->ok = gamelist_set_fields(xml, dir, path, e.fields, res->err);
          for (const std::string& cn : e.colls) {
            std::string er;
            collections::add_game(cn, path, er);
          }
        },
        [=] {
          drop_item(item);
          if (!res->ok) {
            std::fprintf(stderr, "zfront: web manage: gamelist entry not restored for %s: %s\n", path.c_str(), res->err.c_str());
            c->sv->toasts.push(tr("COULD NOT SAVE GAME DATA"), 4000);
          }
          collections::set_reload_needed(true);
          if (reload) reload();  // the game folder is read again: the game is back in the list
          if (refresh) refresh();
        });
  });
  return reply(202, "{\"ok\":true}");
}

http::Response empty_route(const http::Request& req, const Env& env) {
  std::string c;
  if (!param(req, "confirm", c) || c != "1") return err(400, "confirm=1 is needed");
  std::lock_guard<std::mutex> lk(g_mutex);
  size_t n = 0;
  std::set<std::string> roots;
  for (const SystemInfo& si : systems_snapshot()) {
    const std::string root = join_root(si.rom_dir);
    if (root.empty() || !roots.insert(root).second) continue;
    std::vector<std::string> names;
    if (DIR* d = ::opendir(trash_of(root).c_str())) {
      while (dirent* de = ::readdir(d))
        if (item_name_ok(de->d_name)) names.push_back(de->d_name);
      ::closedir(d);
    }
    for (const std::string& name : names) {
      remove_tree(trash_of(root) + "/" + name);
      ++n;
    }
    ::rmdir(trash_of(root).c_str());  // only when nothing else is in it
  }
  (void)env;
  return reply(200, "{\"removed\":" + std::to_string(n) + "}");
}

bool control_free(const std::string& s) {
  return std::none_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
}

std::string states_dir(const std::string& root, const std::string& system) {
  if (const char* e = std::getenv("ZF_MANAGE_STATES")) return std::string(e) + "/" + system;
  return dir_of(root) + "/savestates/" + system;  // as savestates::state_dir
}

http::Response rename_route(const http::Request& req, const Env& env) {
  Target t;
  http::Response deny;
  if (!target_of(req, env, t, deny, true)) return deny;
  std::string name, title;
  param(req, "name", name);
  const bool has_title = param(req, "title", title);
  if (!valid_name(name)) return err(400, "invalid file name");
  const std::string old_file = base_of(t.rel);
  if (name == old_file) return err(400, "that is the current name");
  if (ext_of(name) != ext_of(old_file) || ext_of(name).empty()) return err(400, "the file extension cannot change");
  if (has_title && (title.empty() || title.size() > 200 || !control_free(title))) return err(400, "invalid title");
  std::lock_guard<std::mutex> lk(g_mutex);
  const FoundGame& f = t.f;
  const std::string dir = dir_of(f.path), new_path = dir + "/" + name;
  const std::string old_stem = stem_of(old_file), new_stem = stem_of(name);
  const std::string new_rel = dir_of(t.rel).empty() ? name : dir_of(t.rel) + "/" + name;
  if (int rc = rename_noreplace(f.path, new_path)) {
    if (rc == EEXIST) return err(409, "a file with that name already exists");
    return err(500, "could not rename the file");
  }
  size_t moved = 1;
  // saves next to the ROM
  for (const char* x : kSaveExts) {
    const std::string from = dir + "/" + old_stem + "." + x;
    if (is_file_nolink(from) && rename_noreplace(from, dir + "/" + new_stem + "." + x) == 0) ++moved;
  }
  // save states: <states>/<system>/<stem>.state*, with their pictures
  const std::string sd = states_dir(t.root, f.system);
  std::vector<std::string> st;
  if (DIR* d = ::opendir(sd.c_str())) {
    const std::string pre = old_stem + ".state";
    while (dirent* de = ::readdir(d)) {
      const std::string n = de->d_name;
      if (n.size() >= pre.size() && n.compare(0, pre.size(), pre) == 0) st.push_back(n);  // Foo.state itself, Foo.state1, Foo.state.png...
    }
    ::closedir(d);
  }
  for (const std::string& n : st)
    if (is_file_nolink(sd + "/" + n) && rename_noreplace(sd + "/" + n, sd + "/" + new_stem + n.substr(old_stem.size())) == 0) ++moved;
  // media the gamelist names and that are named after the ROM
  Edit ed;
  ed.sys = f.system;
  ed.old_path = f.path;
  ed.new_path = new_path;
  const bool has_entry = f.meta.span_end != 0;
  ed.rom_key = has_entry ? f.path : new_path;
  if (has_entry) {
    ed.fields.emplace_back("path", "./" + new_rel);
    std::vector<std::string> mt(std::begin(kMedia), std::end(kMedia));
    std::map<std::string, std::string> renamed;  // old absolute -> new absolute (two fields can name one file)
    for (const auto& kv : gamelist_read_fields(f.gamelist, f.rom_dir, f.path, f.meta, mt)) {
      const std::string abs = resolve_rel_path(kv.second, f.rom_dir), b = base_of(abs);
      if (kv.second.size() < b.size() || kv.second.compare(kv.second.size() - b.size(), b.size(), b) != 0) continue;
      if (b.size() <= old_stem.size() || b.compare(0, old_stem.size(), old_stem) != 0 || (b[old_stem.size()] != '-' && b[old_stem.size()] != '.')) continue;
      if (!plain_rel(rel_under(t.root, abs)) || !inside_real(t.root, abs)) continue;
      const std::string nb = new_stem + b.substr(old_stem.size()), nabs = dir_of(abs) + "/" + nb;
      if (!renamed.count(abs)) {
        if (!is_file_nolink(abs) || rename_noreplace(abs, nabs) != 0) continue;
        renamed[abs] = nabs;
        ++moved;
      }
      ed.fields.emplace_back(kv.first, kv.second.substr(0, kv.second.size() - b.size()) + nb);
    }
  }
  if (has_title) {
    ed.title = title;
    ed.fields.emplace_back("name", title);
  }
  run_edit(env, ed);
  return reply(202, "{\"ok\":true,\"moved\":" + std::to_string(moved) + "}");
}

// ---- the picture (streamed) --------------------------------------------------------------------------------------------
/// "png", "jpg" or "webp" by the first bytes; empty when it is none of them.
std::string picture_kind(const std::string& h) {
  if (h.size() >= 8 && h.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) return "png";
  if (h.size() >= 3 && h.compare(0, 3, "\xff\xd8\xff") == 0) return "jpg";
  if (h.size() >= 12 && h.compare(0, 4, "RIFF") == 0 && h.compare(8, 4, "WEBP") == 0) return "webp";
  return std::string();
}

http::Response picture_route(const http::Request& req, http::BodyStream& body, const Env& env) {
  if (!enabled()) return err(404, "not found");
  Target t;
  http::Response deny;
  if (!target_of(req, env, t, deny, false)) return deny;
  const uint64_t len = body.total();
  if (len < 16) return err(400, "that is not a picture");
  if (len > kMaxPicture) return err(413, "the picture is larger than 8 MB");
  std::lock_guard<std::mutex> lk(g_mutex);
  const FoundGame& f = t.f;
  if (!make_dirs(t.root, "images")) return err(500, "cannot create the images folder");
  const std::string stem = stem_of(base_of(t.rel));
  body.accept();
  std::string pending, kind, part, final_path;  // pending: the first bytes, held until they say what the picture is
  int fd = -1;
  std::vector<char> buf(64 * 1024);
  uint64_t written = 0;
  auto bail = [&](http::Response r) {
    if (fd >= 0) ::close(fd);
    if (!part.empty()) ::unlink(part.c_str());
    return r;
  };
  auto put = [&](const char* p, size_t left) {
    while (left > 0) {
      ssize_t w = ::write(fd, p, left);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) return false;
      p += w;
      left -= size_t(w);
    }
    return true;
  };
  uint64_t got = 0;
  for (;;) {
    ptrdiff_t n = body.read(buf.data(), buf.size());
    if (n == 0) break;
    if (n < 0) return bail(err(body.timed_out() ? 408 : 400, "upload interrupted"));
    got += uint64_t(n);
    const char* p = buf.data();
    size_t cnt = size_t(n);
    if (fd < 0) {  // the first bytes decide: the type is never taken from the client
      pending.append(p, cnt);
      if (pending.size() < 12 && got < len) continue;
      kind = picture_kind(pending);
      if (kind.empty()) return bail(err(400, "only PNG, JPEG or WebP pictures are accepted"));
      final_path = t.root + "/images/" + stem + "-image." + kind;
      struct stat sb;
      if (::lstat(final_path.c_str(), &sb) == 0 && !S_ISREG(sb.st_mode)) return bail(err(400, "cannot replace that file"));
      part = final_path + ".part";
      ::unlink(part.c_str());
      fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
      if (fd < 0) {
        part.clear();
        return bail(err(500, "cannot create the file"));
      }
      p = pending.data();
      cnt = pending.size();
    }
    if (!put(p, cnt)) return bail(err(errno == ENOSPC ? 413 : 500, errno == ENOSPC ? "not enough free space" : "write failed"));
    written += cnt;
  }
  if (fd < 0 || written != len || ::fsync(fd) != 0) return bail(err(500, "could not save the picture"));
  const int cl = ::close(fd);
  fd = -1;
  if (cl != 0 || ::rename(part.c_str(), final_path.c_str()) != 0) return bail(err(500, "could not save the picture"));

  Edit ed;
  ed.sys = f.system;
  ed.rom_key = f.path;
  ed.old_path = f.path;  // new_path stays empty: the game itself does not change
  const std::string rel = "./images/" + stem + "-image." + kind;
  ed.fields = {{"image", rel}, {"thumbnail", rel}};  // the phone's cover prefers <thumbnail>
  run_edit(env, ed);
  return reply(202, "{\"ok\":true,\"image\":" + quoted(rel.substr(2)) + "}");
}

// ---- find the picture online ----------------------------------------------------------------------------------------------
http::Response find_route(const http::Request& req, const Env& env) {
  Target t;
  http::Response deny;
  if (!target_of(req, env, t, deny, true)) return deny;
  const std::string arg = mg::game_arg(t.f.system, t.f.path);
  env.on_main([arg](App& app) {
    if (auto s = Registry::instance().make("menu.scraper.game.media", app.cx, arg)) app.push_screen(std::move(s));
  });
  return reply(202, "{\"ok\":true}");
}

// ---- dispatch ------------------------------------------------------------------------------------------------------------
bool owns_path(std::string_view p) {
  return p == "/files/game/info" || p == "/files/game/trash" || p == "/files/game/rename" || p == "/files/game/find" || p == "/files/trash" || p == "/files/trash/restore" ||
         p == "/files/trash/empty" || p == "/files/game/picture";  // the picture: POST is streamed (stream_owns), anything else is 405 here
}

http::Response route(const http::Request& req, const Env& env) {
  const std::string& p = req.path;
  const bool get = p == "/files/game/info" || p == "/files/trash";
  if (req.method != (get ? "GET" : "POST")) return err(405, "method not allowed");
  if (p == "/files/game/info") return info_route(req, env);
  if (p == "/files/game/trash") return trash_route(req, env);
  if (p == "/files/game/rename") return rename_route(req, env);
  if (p == "/files/game/find") return find_route(req, env);
  if (p == "/files/trash") return list_route(req);
  if (p == "/files/trash/restore") return restore_route(req, env);
  if (p == "/files/trash/empty") return empty_route(req, env);
  return err(405, "method not allowed");  // /files/game/picture without the streamed POST
}

const char* const kCss = R"CSS(
#glist .mgmore{flex:none;width:44px;margin:0;padding:.2rem;font-size:1.3rem;line-height:1;background:var(--bg);color:var(--fg);border:1px solid var(--line)}
#mgsheet{position:fixed;inset:0;background:rgba(0,0,0,.6);display:flex;align-items:flex-end;justify-content:center;z-index:6}
#mgcard{background:var(--card);width:100%;max-width:34rem;max-height:88vh;overflow:auto;border-radius:14px 14px 0 0;padding:16px}
#mgcard h2{font-size:1.1rem;margin:0 0 .4rem;word-break:break-word}
#mgcard .hint{font-size:.9rem;opacity:.8;margin:.5rem 0 0}
#mgcard li.mgit{display:flex;gap:10px;align-items:center}
#mgcard li.mgit .nm{flex:1;min-width:0}
#mgcard li.mgit .sy{display:block;font-size:.8rem;opacity:.7}
#mgcard li.mgit button{flex:none;width:auto;margin:0;padding:.4rem .8rem}
#mgnote{min-height:1.4em;font-weight:600}
#mgnote.bad{color:var(--bad)}
)CSS";

const char* const kJs = R"JS(
var $=Z.$;
function el(t,c,x){var e=document.createElement(t);if(c)e.className=c;if(x!==undefined)e.textContent=x;return e}
function btn(label,cls,fn){var b=el("button",cls,label);b.type="button";b.onclick=fn;return b}
function close(){$("mgsheet").hidden=true}
function note(t,bad){var n=$("mgnote");if(n){n.textContent=t;n.className=bad?"bad":""}}
function sheet(title,build){var c=$("mgcard");c.textContent="";c.appendChild(el("h2","",title));build(c);
 var n=el("p");n.id="mgnote";n.setAttribute("role","status");c.appendChild(n);$("mgsheet").hidden=false}
$("mgsheet").onclick=function(e){if(e.target===$("mgsheet"))close()};
function okx(x){return !!x&&(x.status===200||x.status===202)}
function again(){setTimeout(function(){if(Z.games)Z.games()},700)}
function sz(n){return n>=1073741824?(n/1073741824).toFixed(1)+" GB":n>=1048576?(n/1048576).toFixed(1)+" MB":Math.max(1,Math.round(n/1024))+" KB"}
function post(path,done){Z.call("POST",path,function(x){if(!x)return;if(okx(x))done(x);else note(Z.why(x),true)})}

function menu(g){sheet(g.name,function(c){
 c.appendChild(btn("Launch","",function(){close();Z.call("POST","/files/launch?id="+g.id,function(x){
  if(!x)return;if(x.status===200)Z.say("Launching "+g.name+"...");else Z.say(Z.why(x),true)})}));
 c.appendChild(btn("Rename","alt",function(){rename(g)}));
 c.appendChild(btn("Change picture","alt",function(){picture(g)}));
 c.appendChild(btn("Find picture online","alt",function(){post("/files/game/find?id="+g.id,function(){close();
  Z.say("Searching on the console. The picture shows up here when it is done.");setTimeout(function(){if(Z.games)Z.games()},15000)})}));
 c.appendChild(btn("Move to trash","bad",function(){ask(g)}));
 c.appendChild(btn("Cancel","alt",close))})}

function ask(g){sheet("Move to trash?",function(c){
 c.appendChild(el("p","",g.name));
 c.appendChild(el("p","hint","The game file goes to the trash on the console. You can restore it from the Trash button."));
 c.appendChild(btn("Move to trash","bad",function(){post("/files/game/trash?id="+g.id,function(){close();Z.say("Moved to the trash.");again()})}));
 c.appendChild(btn("Cancel","alt",close))})}

function rename(g){Z.call("GET","/files/game/info?id="+g.id,function(x){
 if(!x)return;if(x.status!==200){Z.say(Z.why(x),true);return}
 var i={};try{i=JSON.parse(x.responseText)}catch(_){}
 sheet("Rename",function(c){
  c.appendChild(el("label","","File name"));var f=el("input");f.value=i.file||"";f.maxLength=200;f.setAttribute("autocomplete","off");c.appendChild(f);
  c.appendChild(el("label","","Title shown in the lists"));var t=el("input");t.value=i.title||"";t.maxLength=200;t.setAttribute("autocomplete","off");c.appendChild(t);
  c.appendChild(el("p","hint","The extension cannot change. Saves and save states follow the new file name."));
  c.appendChild(btn("Save","",function(){
   var q="/files/game/rename?id="+g.id+"&name="+encodeURIComponent(f.value);
   if(t.value!==i.title)q+="&title="+encodeURIComponent(t.value);
   post(q,function(){close();Z.say("Renamed.");again()})}));
  c.appendChild(btn("Cancel","alt",close))})})}

function picture(g){sheet("Change picture",function(c){
 var f=el("input");f.type="file";f.accept="image/png,image/jpeg,image/webp";c.appendChild(f);
 c.appendChild(el("p","hint","PNG, JPEG or WebP, up to 8 MB."));
 var b=btn("Upload","",function(){var file=f.files[0];
  if(!file){note("Choose a picture.",true);return}
  if(file.size>8388608){note("The picture is larger than 8 MB.",true);return}
  b.disabled=true;note("Sending...");
  var x=new XMLHttpRequest();x.open("POST","/files/game/picture?id="+g.id);Z.auth(x);
  x.onload=function(){b.disabled=false;if(okx(x)){close();Z.say("Picture saved.");again()}else note(Z.why(x),true)};
  x.onerror=function(){b.disabled=false;note("Cannot reach the console.",true)};
  x.send(file)});
 c.appendChild(b);c.appendChild(btn("Cancel","alt",close))})}

function trash(){Z.call("GET","/files/trash",function(x){
 if(!x)return;if(x.status!==200){Z.say(Z.why(x),true);return}
 var r={};try{r=JSON.parse(x.responseText)}catch(_){}
 var items=r.items||[];
 sheet("Trash",function(c){
  if(!items.length)c.appendChild(el("p","","The trash is empty."));
  var ul=el("ul");
  items.forEach(function(it){var li=el("li","mgit"),sp=el("span","nm",it.name);
   sp.appendChild(el("small","sy",it.system+" - "+sz(it.size)+" - "+it.date));li.appendChild(sp);
   li.appendChild(btn("Restore","alt",function(){post("/files/trash/restore?item="+encodeURIComponent(it.id),function(){
    Z.say("Restored "+it.name+".");trash();again()})}));
   ul.appendChild(li)});
  c.appendChild(ul);
  if(items.length)c.appendChild(btn("Empty trash","bad",function(){sure(items.length)}));
  c.appendChild(btn("Close","alt",close))})})}

function sure(n){sheet("Empty the trash?",function(c){
 c.appendChild(el("p","",n+(n===1?" game":" games")+" will be deleted for good. This cannot be undone."));
 c.appendChild(btn("Empty trash","bad",function(){post("/files/trash/empty?confirm=1",function(){close();Z.say("Trash emptied.")})}));
 c.appendChild(btn("Cancel","alt",trash))})}

var tb=btn("Trash","alt",trash);tb.id="mgtb";$("p-ga").insertBefore(tb,$("quit"));
Z.onrow=function(g,li){var b=btn("…","mgmore",function(e){e.stopPropagation();menu(g)});
 b.setAttribute("aria-label","Actions for "+g.name);li.appendChild(b)};
)JS";

WebModule make_manage() {
  WebModule m;
  m.name = "manage";
  m.order = 60;
  m.owns = owns_path;
  m.route = route;
  m.stream_owns = [](std::string_view method, std::string_view path) { return method == "POST" && path == "/files/game/picture"; };
  m.stream = picture_route;
  m.css = kCss;
  m.html = "<div id=\"mgsheet\" hidden><div id=\"mgcard\"></div></div>";
  m.js = kJs;
  return m;
}

}  // namespace

ZF_REGISTER_WEB_MODULE(make_manage);

}  // namespace webtransfer
}  // namespace zf
