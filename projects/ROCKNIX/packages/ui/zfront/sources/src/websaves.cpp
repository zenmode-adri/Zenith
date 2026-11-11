// MODULE `websaves` (web file transfer, tab "Saves"): download, upload and restore the save backups of LIBRARY > SAVE BACKUPS from a
// phone or PC browser. Everything about the archives (names, what a backup holds, create, restore with the automatic backup first)
// is savebackup.hpp; this file only puts it on the network.
//
//   GET  /files/saves                  {"backups":[{"name","date","bytes"}],"free":N,"busy":B,"game":G}
//   POST /files/saves/create           start a backup on the console (202); one operation at a time (409)
//   GET  /files/saves/status           {"busy":B,"last":{"op","ok","name","error"}|null}
//   GET  /files/saves/get?name=        the archive (Response::file), name = one of the listing
//   POST /files/saves/put?name=        STREAMED upload of a backup made elsewhere; checked before it is listed (422 when it fails)
//   POST /files/saves/restore?name=&confirm=1   replace the saves with that backup (an automatic backup of the current ones first),
//                                               once someone says yes to the question it opens ON THE CONSOLE;
//                                      400 without confirm=1, 409 while a game runs
//
// Safety: the client only ever names an archive of the listing (`savebackup::valid_name`), never a path. The slow work (tar, gzip)
// runs on the console's job lane, started from the main thread (`Env::on_main`), never on an HTTP thread; the menu and this page
// share one busy flag (`savebackup::busy_flag`). An uploaded archive is stored as `<name>.part`, then READ with the tar headers
// parsed here (not by the console's tar, whose listing is neither uniform nor safe against odd names) and refused unless it is a
// gzip stream of regular files and folders only (no link, device or fifo), with no absolute path and no `..`, every path inside a
// location `savebackup::plan` would have archived (`savebackup::in_plan`), at most 4 GB once unpacked. Only then it is renamed to
// its final name; nothing is kept when the check fails. `ZF_WEBSAVES_STORAGE` / `ZF_WEBSAVES_TMP` move the storage root and the
// scratch folder (tests).
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "app.hpp"
#include "i18n.hpp"
#include "menu_system.hpp"
#include "oscontract.hpp"
#include "savebackup.hpp"
#include "services.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"
#include "webcore.hpp"

namespace zf {
namespace {

using webtransfer::err;
using webtransfer::reply;
namespace sb = savebackup;

constexpr uint64_t kMaxUnpacked = 4ull << 30;  // bytes of file data in an uploaded backup
constexpr uint64_t kMaxUpload = 4ull << 30;    // bytes of the .tar.gz itself
constexpr size_t kMaxEntries = 200000;

std::string storage() {
  const char* e = std::getenv("ZF_WEBSAVES_STORAGE");
  return e && *e ? e : "/storage";
}
std::string tmp_dir() {
  const char* e = std::getenv("ZF_WEBSAVES_TMP");
  return e && *e ? e : "/tmp";
}
std::string dir() { return sb::backup_dir(storage()); }

struct Last {
  bool have = false, ok = false;
  std::string op, name, error;
};
std::mutex g_m;
Last g_last;

void set_last(const char* op, bool ok, const std::string& name, const std::string& error) {
  std::lock_guard<std::mutex> lk(g_m);
  g_last = Last{true, ok, op, name, error};
}

bool acquire() { return !sb::busy_flag().exchange(true); }
void release() {
  sb::busy_flag() = false;
  ++sb::generation();
}

void mkdirs(const std::string& path) {
  for (size_t i = 1; i <= path.size(); ++i)
    if (i == path.size() || path[i] == '/') ::mkdir(path.substr(0, i).c_str(), 0755);
}

std::string err_text(sb::Err e) {
  switch (e) {
    case sb::Err::NoSaves: return "no saves found";
    case sb::Err::NoSpace: return "not enough free space";
    case sb::Err::Exists: return "a backup from this minute already exists";
    case sb::Err::Damaged: return "the backup is damaged or missing";
    case sb::Err::PreFailed: return "the backup of the current saves failed";
    default: return "an error occurred";
  }
}

// ---- reading an uploaded archive ---------------------------------------------------------------------------------------------

// Octal field, or the GNU base-256 form; false when it is neither (or negative).
bool parse_num(const unsigned char* p, size_t n, uint64_t& out) {
  if (p[0] & 0x80) {
    if (p[0] & 0x40) return false;
    uint64_t v = p[0] & 0x3f;
    for (size_t i = 1; i < n; ++i) {
      if (v >> 55) return false;
      v = (v << 8) | p[i];
    }
    out = v;
    return true;
  }
  uint64_t v = 0;
  size_t i = 0;
  while (i < n && (p[i] == ' ' || p[i] == 0)) ++i;
  bool any = false;
  for (; i < n && p[i] >= '0' && p[i] <= '7'; ++i) {
    if (v >> 60) return false;
    v = (v << 3) | uint64_t(p[i] - '0');
    any = true;
  }
  for (; i < n; ++i)
    if (p[i] != ' ' && p[i] != 0) return false;
  out = any ? v : 0;
  return true;
}

std::string cstr(const unsigned char* p, size_t n) {
  size_t k = 0;
  while (k < n && p[k]) ++k;
  return std::string(reinterpret_cast<const char*>(p), k);
}

// A path of the archive: relative, plain components, inside a save location.
bool path_ok(std::string name, bool is_dir) {
  if (is_dir && !name.empty() && name.back() == '/') name.pop_back();
  if (name.empty() || name.size() > 4096 || name[0] == '/') return false;
  size_t a = 0;
  while (a <= name.size()) {
    size_t e = name.find('/', a);
    if (e == std::string::npos) e = name.size();
    const std::string c = name.substr(a, e - a);
    if (c.empty() || c == "." || c == "..") return false;
    a = e + 1;
  }
  for (unsigned char c : name)
    if (c < 0x20 || c == 0x7f) return false;
  return sb::in_plan(name, is_dir);
}

// "<len> key=value\n" records of a pax header. The console's tar (BusyBox) IGNORES pax headers: it extracts every entry under the
// name and with the size of its plain header. So a record that would rename or resize the entry (`path`, `linkpath`, `size`) is
// refused: the archive would not be restored the way it reads here. The others (times, owner) change nothing about where data goes.
bool parse_pax(const std::string& d) {
  size_t a = 0;
  while (a < d.size()) {
    size_t sp = d.find(' ', a);
    if (sp == std::string::npos || sp == a) return false;
    uint64_t len = 0;
    for (size_t i = a; i < sp; ++i) {
      if (d[i] < '0' || d[i] > '9' || len > d.size()) return false;
      len = len * 10 + uint64_t(d[i] - '0');
    }
    if (len < sp - a + 3 || a + len > d.size() || d[a + len - 1] != '\n') return false;
    const std::string rec = d.substr(sp + 1, a + len - 1 - (sp + 1));
    size_t eq = rec.find('=');
    if (eq == std::string::npos) return false;
    const std::string key = rec.substr(0, eq);
    if (key == "path" || key == "linkpath" || key == "size") return false;
    a += len;
  }
  return true;
}

struct Pipe {
  FILE* f = nullptr;
  uint64_t got = 0;
  ~Pipe() {
    if (f) ::pclose(f);
  }
  bool read(void* buf, size_t n) {
    if (std::fread(buf, 1, n, f) != n) return false;
    got += n;
    return true;
  }
  bool skip(uint64_t n) {
    char b[8192];
    while (n > 0) {
      size_t k = size_t(n < sizeof b ? n : sizeof b);
      if (!read(b, k)) return false;
      n -= k;
    }
    return true;
  }
};

/// Reads `path` (a .tar.gz) and says whether it is a backup this console may restore. `why` holds the reason when not.
bool check_archive(const std::string& path, std::string& why) {
  {  // gzip magic, read here: a text file would otherwise only show as a failing gzip
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    unsigned char m[2] = {0, 0};
    const bool ok = fd >= 0 && ::read(fd, m, 2) == 2 && m[0] == 0x1f && m[1] == 0x8b;
    if (fd >= 0) ::close(fd);
    if (!ok) {
      why = "not a gzip file";
      return false;
    }
  }
  Pipe p;
  p.f = ::popen(("gzip -dc < " + shell_quote(path) + " 2>/dev/null").c_str(), "re");
  if (!p.f) {
    why = "cannot read the file";
    return false;
  }
  uint64_t total = 0;
  size_t entries = 0, regular = 0;
  std::string long_name;
  bool have_long = false;
  bool clean_end = false;
  for (;;) {
    unsigned char h[512];
    if (!p.read(h, sizeof h)) break;
    if (std::all_of(h, h + sizeof h, [](unsigned char c) { return c == 0; })) {
      clean_end = true;
      break;
    }
    uint64_t sum = 0, want = 0;
    for (size_t i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    if (!parse_num(h + 148, 8, want) || want != sum) {
      why = "not a tar archive (bad header)";
      return false;
    }
    if (++entries > kMaxEntries) {
      why = "too many entries";
      return false;
    }
    uint64_t size = 0;
    if (!parse_num(h + 124, 12, size)) {
      why = "not a tar archive (bad size)";
      return false;
    }
    const char type = char(h[156]);
    const bool ustar = std::memcmp(h + 257, "ustar", 5) == 0;
    std::string name = cstr(h, 100);
    if (ustar && h[345]) name = cstr(h + 345, 155) + "/" + name;
    if (type == 'g') {  // a global pax header would apply to every entry after it
      why = "unsupported archive header (pax global): make the archive with plain tar";
      return false;
    }
    if (type == 'x' || type == 'L') {
      if (size > 65536) {
        why = "unsupported archive header";
        return false;
      }
      std::string d(size_t(size), '\0');
      if (size && !p.read(&d[0], size_t(size))) break;
      if (!p.skip((512 - size % 512) % 512)) break;
      if (type == 'L') {
        long_name = d.substr(0, d.find('\0'));
        have_long = true;
      } else if (type == 'x') {
        if (!parse_pax(d)) {
          why = "unsupported archive header (pax path or size): make the archive with plain tar";
          return false;
        }
      }
      continue;
    }
    if (have_long) name = long_name;
    have_long = false;
    const bool reg = type == '0' || type == '\0';
    const bool dirent = type == '5';
    if (!reg && !dirent) {
      why = type == '1' || type == '2' ? "the archive contains a link" : "the archive contains a special file";
      return false;
    }
    if (!path_ok(name, dirent)) {
      why = "the archive holds a path that is not a save location: " + name.substr(0, 80);
      return false;
    }
    if (reg) {
      ++regular;
      if (size > kMaxUnpacked || total + size > kMaxUnpacked) {
        why = "the archive is too big (over 4 GB unpacked)";
        return false;
      }
      total += size;
    } else {
      size = 0;
    }
    if (!p.skip(size) || !p.skip((512 - size % 512) % 512)) break;
    if (p.got > kMaxUnpacked + (64u << 20)) {
      why = "the archive is too big (over 4 GB unpacked)";
      return false;
    }
  }
  if (clean_end) {  // the rest (padding) up to the end of the stream, so that gzip can say whether the whole file was sound
    char b[8192];
    uint64_t extra = 0;
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, p.f)) > 0 && (extra += n) < (64u << 20)) {
    }
  }
  const int rc = ::pclose(p.f);
  p.f = nullptr;
  if (!clean_end || rc != 0) {
    why = "the archive is damaged or cut short";
    return false;
  }
  if (regular == 0) {
    why = "the archive holds no saves";
    return false;
  }
  return true;
}

// ---- routes ------------------------------------------------------------------------------------------------------------------

bool listed(const std::string& name) {
  if (!sb::valid_name(name)) return false;
  for (const sb::Backup& b : sb::list(dir()))
    if (b.name == name) return true;
  return false;
}

http::Response list_route() {
  std::string j = "{\"backups\":[";
  bool first = true;
  for (const sb::Backup& b : sb::list(dir())) {
    j += std::string(first ? "" : ",") + "{\"name\":\"" + oscontract::json_escape(b.name) + "\",\"date\":\"" + oscontract::json_escape(sb::pretty_date(b.name)) +
         "\",\"bytes\":" + std::to_string(b.bytes) + "}";
    first = false;
  }
  j += "],\"free\":" + std::to_string(sb::free_bytes(dir())) + ",\"busy\":" + (sb::busy_flag() ? "true" : "false") +
       ",\"game\":" + (oscontract::game_running() ? "true" : "false") + "}";
  return reply(200, j);
}

http::Response status_route() {
  std::lock_guard<std::mutex> lk(g_m);
  std::string j = std::string("{\"busy\":") + (sb::busy_flag() ? "true" : "false") + ",\"last\":";
  if (!g_last.have) j += "null";
  else
    j += "{\"op\":\"" + oscontract::json_escape(g_last.op) + "\",\"ok\":" + (g_last.ok ? "true" : "false") + ",\"name\":\"" + oscontract::json_escape(g_last.name) +
         "\",\"error\":\"" + oscontract::json_escape(g_last.error) + "\"}";
  return reply(200, j + "}");
}

// Runs on the main thread: queues the work on the slow lane; the result is recorded where the job finishes.
void start_job(Ctx& cx, bool restore, const std::string& name, const std::function<void()>& reload) {
  auto sv = cx.sv;
  auto res = std::make_shared<sb::Result>();
  const std::string st = storage(), tmp = tmp_dir(), stamp = sb::stamp_now();
  const char* op = restore ? "restore" : "create";
  msys::post(cx, [sv, res, st, tmp, stamp, restore, name] { *res = restore ? sb::restore(*sv, st, tmp, stamp, name) : sb::create(*sv, st, tmp, stamp, false); },
             [sv, res, op, restore, reload] {
               const bool ok = res->err == sb::Err::None;
               set_last(op, ok, ok ? res->name : std::string(), ok ? std::string() : err_text(res->err));
               release();
               sv->toasts.push(ok ? std::string(restore ? tr("SAVES RESTORED") : tr("BACKUP DONE")) : std::string(tr("AN ERROR OCCURRED")), ok ? 4000 : 6000);
               if (ok && restore && reload) reload();  // what the games show about their saves (the continue row) comes from the files just replaced
             },
             true);
}

/// RESTORE SAVES FROM THE WEB? asked on the console: a phone alone (its PIN, its pairing) cannot replace the saves, someone holding
/// the console has to say yes. No is selected; closing it in any way but Yes is a No. The backup flag stays taken while it is open.
class RestoreDialog : public ui::ConfirmDialog {
 public:
  RestoreDialog(const std::string& name, std::function<void()> reload, std::shared_ptr<bool> said, ui::ConfirmOpts o)
      : ui::ConfirmDialog(name,
                          [name, reload, said](Ctx& cx) {
                            *said = true;
                            if (oscontract::game_running()) {  // started while the question waited
                              set_last("restore", false, "", "a game is running");
                              release();
                            } else {
                              start_job(cx, true, name, reload);
                            }
                            return Transition::none();
                          },
                          std::move(o), false),
        said_(std::move(said)) {}
  ~RestoreDialog() override {
    if (*said_) return;
    set_last("restore", false, "", "declined on the console");
    release();
  }

 private:
  std::shared_ptr<bool> said_;
};

http::Response start_route(const webtransfer::Env& env, bool restore, const std::string& name) {
  if (!env.on_main) return err(500, "not available");
  if (!acquire()) return err(409, "another backup operation is running");
  auto reload = env.reload;
  env.on_main([restore, name, reload](App& app) {
    if (!restore) {
      start_job(app.cx, false, name, reload);
      return;
    }
    ui::ConfirmOpts o;
    o.title = tr("RESTORE SAVES FROM THE WEB?");
    o.danger = true;
    app.push_screen(std::make_unique<RestoreDialog>(name, reload, std::make_shared<bool>(false), std::move(o)));
  });
  return reply(202, "{\"busy\":true}");
}

http::Response get_route(const http::Request& req) {
  std::string name;
  if (!http::query_param(req, "name", name) || !sb::valid_name(name)) return err(400, "invalid backup name");
  if (!listed(name)) return err(404, "no such backup");
  struct stat st {};
  const std::string path = dir() + "/" + name;
  if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return err(404, "no such backup");
  http::Response r;
  r.status = 200;
  r.content_type = "application/gzip";
  r.file = path;
  r.headers.push_back({"Content-Disposition", "attachment; filename=\"" + name + "\""});
  r.headers.push_back({"Cache-Control", "no-store"});
  return r;
}

http::Response restore_route(const http::Request& req, const webtransfer::Env& env) {
  std::string name, confirm;
  if (!http::query_param(req, "name", name) || !sb::valid_name(name)) return err(400, "invalid backup name");
  if (!http::query_param(req, "confirm", confirm) || confirm != "1") return err(400, "confirm=1 is required");
  if (oscontract::game_running()) return err(409, "a game is running");
  if (!listed(name)) return err(404, "no such backup");
  return start_route(env, true, name);
}

http::Response route(const http::Request& req, const webtransfer::Env& env) {
  const std::string& p = req.path;
  const bool is_post = p == "/files/saves/create" || p == "/files/saves/restore";
  if (req.method != (is_post ? "POST" : "GET")) return err(405, "method not allowed");
  if (p == "/files/saves") return list_route();
  if (p == "/files/saves/status") return status_route();
  if (p == "/files/saves/get") return get_route(req);
  if (p == "/files/saves/create") return start_route(env, false, "");
  return restore_route(req, env);
}

http::Response put(const http::Request& req, http::BodyStream& body, const webtransfer::Env&) {
  std::string name;
  if (!http::query_param(req, "name", name) || !sb::valid_name(name)) return err(400, "invalid backup name");
  const uint64_t len = body.total();
  if (len == 0) return err(400, "empty file");
  if (len > kMaxUpload) return err(413, "the file is too big");
  const std::string d = dir(), final_path = d + "/" + name, part = final_path + ".part";
  struct stat sbuf {};
  if (::lstat(final_path.c_str(), &sbuf) == 0) return err(409, "a backup with that name already exists");
  if (!acquire()) return err(409, "another backup operation is running");
  struct Guard {
    ~Guard() { release(); }
  } guard;
  mkdirs(d);
  if (sb::free_bytes(d) < len + 65536) return err(413, "not enough free space");
  ::unlink(part.c_str());  // leftover of an interrupted upload (only one runs at a time)
  int fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (fd < 0) return err(500, "cannot create the file");
  body.accept();
  std::vector<char> buf(64 * 1024);
  uint64_t written = 0;
  for (;;) {
    ptrdiff_t n = body.read(buf.data(), buf.size());
    if (n == 0) break;
    if (n < 0) {
      ::close(fd);
      ::unlink(part.c_str());
      return err(body.timed_out() ? 408 : 400, "upload interrupted");
    }
    const char* q = buf.data();
    size_t left = size_t(n);
    while (left > 0) {
      ssize_t w = ::write(fd, q, left);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) {
        const bool full = errno == ENOSPC;
        ::close(fd);
        ::unlink(part.c_str());
        return err(full ? 413 : 500, full ? "not enough free space" : "write failed");
      }
      q += w;
      left -= size_t(w);
    }
    written += uint64_t(n);
  }
  const bool synced = ::fsync(fd) == 0;
  const bool closed = ::close(fd) == 0;
  if (!synced || !closed || written != len) {
    ::unlink(part.c_str());
    return err(500, "could not save the file");
  }
  std::string why;
  if (!check_archive(part, why)) {
    ::unlink(part.c_str());
    set_last("upload", false, name, why);
    return reply(422, "{\"error\":\"" + oscontract::json_escape(why) + "\"}");
  }
  if (::lstat(final_path.c_str(), &sbuf) == 0) {  // appeared while we were receiving
    ::unlink(part.c_str());
    return err(409, "a backup with that name already exists");
  }
  if (::rename(part.c_str(), final_path.c_str()) != 0) {
    ::unlink(part.c_str());
    return err(500, "could not save the file");
  }
  set_last("upload", true, name, "");
  return reply(200, "{\"ok\":true,\"name\":\"" + oscontract::json_escape(name) + "\",\"bytes\":" + std::to_string(written) + "}");
}

// ---- the page ----------------------------------------------------------------------------------------------------------------

const char* kCss = R"CSS(#sv li{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
#sv .nm{flex:1;min-width:9rem}#sv .nm small{display:block;opacity:.7}
#sv li button{width:auto;margin:0;padding:.4rem .8rem}
#svinfo{font-size:.9rem;opacity:.85;margin:.5rem 0 0})CSS";

const char* kSection = R"HTML(<button type="button" id="svnow">Back up now</button>
<p id="svinfo"></p>
<ul id="sv"></ul>
<label for="svfile">Upload a backup (.tar.gz)</label>
<input id="svfile" type="file" accept=".gz,.tgz,application/gzip">
<button type="button" id="svup" class="alt">Upload</button>)HTML";

const char* kJs = R"JS(var list=[],game=false,busy=false,timer=0,upBusy=false;
function note(t){Z.$("svinfo").textContent=t}
function human(n){return n>=1073741824?(n/1073741824).toFixed(1)+" GB":n>=1048576?(n/1048576).toFixed(1)+" MB":n>=1024?Math.round(n/1024)+" KB":n+" B"}
function draw(){var ul=Z.$("sv");ul.textContent="";
 if(!list.length){var e=document.createElement("li");e.textContent="No backups yet.";ul.appendChild(e)}
 list.forEach(function(b){var li=document.createElement("li"),nm=document.createElement("span"),sm=document.createElement("small"),
  d=document.createElement("button"),r=document.createElement("button");
  nm.className="nm";nm.textContent=b.date;sm.textContent=human(b.bytes);nm.appendChild(sm);
  d.type="button";d.textContent="Download";d.className="alt";d.disabled=busy;
  r.type="button";r.textContent="Restore";r.disabled=busy||game;if(game)r.title="A game is running.";
  d.onclick=function(){fetchFile(b)};r.onclick=function(){restore(b)};
  li.appendChild(nm);li.appendChild(d);li.appendChild(r);ul.appendChild(li)});
 Z.$("svnow").disabled=busy||upBusy;Z.$("svup").disabled=busy||upBusy}
function fetchFile(b){Z.say("");note("Downloading...");
 Z.call("GET","/files/saves/get?name="+encodeURIComponent(b.name),function(x){note("");
  if(!x)return;if(x.status!==200){Z.say(Z.why(x),true);return}
  var u=URL.createObjectURL(x.response),a=document.createElement("a");a.setAttribute("href",u);a.download=b.name;
  document.body.appendChild(a);a.click();document.body.removeChild(a);setTimeout(function(){URL.revokeObjectURL(u)},60000)},true)}
function restore(b){if(game||busy)return;
 if(!window.confirm("Replace the saves on the console with this backup? A backup of the current saves is made first."))return;
 Z.say("");Z.call("POST","/files/saves/restore?name="+encodeURIComponent(b.name)+"&confirm=1",function(x){
  if(!x)return;if(x.status!==202){Z.say(Z.why(x),true);return}
  busy=true;draw();note("Confirm on the console...");poll()})}
function poll(){clearTimeout(timer);timer=setTimeout(function(){Z.call("GET","/files/saves/status",function(x){
 if(!x||x.status!==200){poll();return}
 var s={};try{s=JSON.parse(x.responseText)}catch(_){}
 if(s.busy){poll();return}
 busy=false;var l=s.last;
 if(l&&(l.op==="create"||l.op==="restore")){if(l.ok)Z.say(l.op==="restore"?"Saves restored.":"Backup done: "+l.name);else Z.say("Failed: "+l.error,true)}
 note("");refresh()})},1000)}
function refresh(){Z.call("GET","/files/saves",function(x){
 if(Z.jerr(x))return;var r={};try{r=JSON.parse(x.responseText)}catch(_){}
 list=r.backups||[];game=!!r.game;busy=!!r.busy;draw();
 note((game?"A game is running: restoring is off until you close it. ":"")+"Free space: "+human(r.free||0));
 if(busy){note("Working on the console...");poll()}})}
Z.$("svnow").onclick=function(){if(busy)return;Z.say("");
 Z.call("POST","/files/saves/create",function(x){if(!x)return;if(x.status!==202){Z.say(Z.why(x),true);return}
  busy=true;draw();note("Backing up...");poll()})};
Z.$("svup").onclick=function(){var f=Z.$("svfile").files[0];if(busy||upBusy)return;
 if(!f){Z.say("Choose a backup file first.",true);return}
 if(!/^saves-\d{8}-\d{4}(-pre\d*)?\.tar\.gz$/.test(f.name)){Z.say("The file name must look like saves-YYYYMMDD-HHMM.tar.gz.",true);return}
 Z.say("");upBusy=true;draw();note("Uploading 0%");
 var x=new XMLHttpRequest();x.open("POST","/files/saves/put?name="+encodeURIComponent(f.name));Z.auth(x);
 x.upload.onprogress=function(e){if(e.lengthComputable)note("Uploading "+Math.floor(100*e.loaded/e.total)+"%")};
 x.onload=function(){upBusy=false;note("");if(x.status===200){Z.say("Backup uploaded.");Z.$("svfile").value=""}else Z.say(Z.why(x),true);refresh()};
 x.onerror=function(){upBusy=false;note("");Z.say("Connection lost.",true);refresh()};
 x.send(f)};
Z.ontab.sv=function(){refresh()};)JS";

}  // namespace

ZF_REGISTER_WEB_MODULE([] {
  webtransfer::WebModule m;
  m.name = "saves";
  m.order = 60;
  m.owns = [](std::string_view p) {
    return p == "/files/saves" || p == "/files/saves/create" || p == "/files/saves/status" || p == "/files/saves/get" || p == "/files/saves/restore";
  };
  m.route = route;
  m.stream_owns = [](std::string_view meth, std::string_view p) { return meth == "POST" && p == "/files/saves/put"; };
  m.stream = put;
  m.tab_id = "sv";
  m.tab_label = "Saves";
  m.css = kCss;
  m.section = kSection;
  m.js = kJs;
  return m;
});

}  // namespace zf
