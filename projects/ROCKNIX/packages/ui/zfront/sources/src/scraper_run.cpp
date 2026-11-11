#include "scraper_run.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>
#include <unordered_map>

#include "gamemeta.hpp"
#include "oscontract.hpp"
#include "pugixml.hpp"
#include "scraper_adb.hpp"
#include "scraper_hash.hpp"
#include "scraper_igdb.hpp"
#include "systems.hpp"
#include "util.hpp"

namespace zf::scraper {

bool available() { return available(Source::ScreenScraper); }

namespace {
std::string iso_local(time_t t) {
  struct tm tm {};
  ::localtime_r(&t, &tm);
  char b[24];
  std::strftime(b, sizeof b, "%Y%m%dT%H%M%S", &tm);
  return b;
}
}  // namespace

// ---- media kinds (ES `Scraper::getSaveAsPath`: <rom dir>/<folder>/<rom stem>-<suffix>.<ext>) ------------------------------
namespace {

struct Kind {
  const char* kind;    // ss::Pick::kind
  const char* tag;     // gamelist.xml child
  const char* folder;
  const char* suffix;
  bool resize;         // the server may shrink it (ScraperResizeWidth)
};
constexpr Kind kKinds[] = {
    {"image", "image", "images", "image", true},       {"thumb", "thumbnail", "images", "thumb", true},
    {"marquee", "marquee", "images", "marquee", true}, {"video", "video", "videos", "video", false},
    {"fanart", "fanart", "images", "fanart", false},   {"boxback", "boxback", "images", "boxback", false},
    {"titleshot", "titleshot", "images", "titleshot", true}, {"manual", "manual", "manuals", "manual", false},
    {"map", "map", "images", "map", false}};
constexpr size_t kKindCount = sizeof kKinds / sizeof *kKinds;

int kind_index(const std::string& k) {
  for (size_t i = 0; i < kKindCount; ++i)
    if (k == kKinds[i].kind) return int(i);
  return -1;
}

bool kind_wanted(size_t i, const ss::Wish& w) {
  switch (i) {
    case 0: return !w.image_src.empty();
    case 1: return !w.thumb_src.empty();
    case 2: return !w.logo_src.empty();
    case 3: return w.video;
    case 4: return w.fanart;
    case 5: return w.boxback;
    case 6: return w.titleshot;
    case 7: return w.manual;
    case 8: return w.map;
  }
  return false;
}

// ES `getSupportedMedias()`: IGDB gives screenshot, box and fan art; ArcadeDB screenshot, box, marquee, title shot and video
bool kind_in_source(Source s, size_t i) {
  switch (s) {
    case Source::IGDB: return i == 0 || i == 1 || i == 4;
    case Source::ArcadeDB: return i == 0 || i == 1 || i == 2 || i == 3 || i == 6;
    default: return true;
  }
}

}  // namespace

// ---- options ----------------------------------------------------------------------------------------------------------
Options Options::load(const Ctx& cx) {
  Options o;
  auto str = [&](const char* k, const char* d) {
    const std::string* v = cx.es.get(k);
    return v ? *v : std::string(d);
  };
  o.wish.image_src = str(key::kImage, "ss");
  o.wish.thumb_src = str(key::kThumb, "box-2D");
  o.wish.logo_src = str(key::kLogo, "wheel");
  o.wish.region = str(key::kRegion, "");
  o.wish.ratings = cx.es.get_bool(key::kRatings, true);
  o.wish.video = cx.es.get_bool(key::kVideos, true);
  o.wish.fanart = cx.es.get_bool(key::kFanart, false);
  o.wish.boxback = cx.es.get_bool(key::kBoxBack, false);
  o.wish.manual = cx.es.get_bool(key::kManual, true);
  o.wish.map = cx.es.get_bool(key::kMap, true);
  o.overwrite_names = cx.es.get_bool(key::kNames, true);
  o.overwrite_desc = cx.es.get_bool(key::kDesc, true);
  o.overwrite_media = cx.es.get_bool(key::kOverwrite, true);
  long w = cx.es.get_int(key::kResizeW, 640), h = cx.es.get_int(key::kResizeH, 0);
  o.resize_px = int(w > 0 ? w : (h > 0 ? h : 0));
  o.user = str(key::kUser, "");
  o.pass = str(key::kPass, "");
  o.locale = cx.conf.get_or("system.language", "");
  o.source = source_of(cx.es);
  o.igdb_id = str(key::kIgdbId, "");
  o.igdb_secret = str(key::kIgdbSecret, "");
  o.short_title = cx.es.get_bool(key::kShortTitle, false);
  static const int kDays[] = {0, 1, 7, 15, 31, 90, 365};  // ES's "IGNORE RECENTLY SCRAPED GAMES" list
  long idx = cx.es.get_int("RecentlyScrappedFilter", 3);
  o.skip_days = kDays[idx >= 0 && idx < 7 ? idx : 3];
  return o;
}

const char* describe(Err e) {
  switch (e) {
    case Err::Network: return tr("NETWORK ERROR");
    case Err::Busy: return tr("SERVER BUSY");
    case Err::Server: return tr("SERVER ERROR");
    case Err::Write: return tr("CANNOT WRITE THE GAMELIST");
    default: return "";
  }
}

const char* describe(Abort a) {
  switch (a) {
    case Abort::BadLogin: return tr("LOGIN FAILED. CHECK YOUR USERNAME AND PASSWORD");
    case Abort::ApiClosed: return tr("SCREENSCRAPER IS CLOSED TO THIS ACCOUNT RIGHT NOW");
    case Abort::Maintenance: return tr("SCREENSCRAPER IS UNDER MAINTENANCE");
    case Abort::Blacklisted: return tr("THIS SOFTWARE IS BLOCKED BY SCREENSCRAPER");
    case Abort::Quota: return tr("DAILY SCRAPE QUOTA REACHED. TRY AGAIN TOMORROW");
    case Abort::TooManyFails: return tr("TOO MANY UNRECOGNISED ROMS TODAY. TRY AGAIN TOMORROW");
    case Abort::NoNetwork: return tr("NO NETWORK CONNECTION");
    case Abort::DiskFull: return tr("NO SPACE LEFT ON THE CARD");
    case Abort::InvalidCredentials: return tr("INVALID CREDENTIALS");
    case Abort::NoCredentials: return tr("TYPE THE IGDB CLIENT ID AND SECRET IN ACCOUNT FIRST");
    default: return "";
  }
}

// ---- worker side ------------------------------------------------------------------------------------------------------
struct SysInfo {
  std::string name, display, rom_dir, gamelist, platform;
  std::vector<std::string> exts;
  std::vector<int> ids;
  bool extract = true;
};

struct Item {
  uint32_t sys = 0;  // position in Options::systems
  std::string path, name;
  std::string md5, crc;  // from the gamelist
  bool has_name = false, has_desc = false;
  std::string cur[kKindCount];  // resolved path of the media the gamelist names AND that exists (else empty)
};

struct Env {
  Options o;
  ss::Login login;
  std::string tmp, curl;
  std::string tok;           // IGDB access token (worker thread only)
  TimePoint tok_until{};
  std::vector<SysInfo> sys;
  bool overwrite_media = true;
  std::shared_ptr<Services> sv;
  unsigned seq = 0;
  std::atomic<int> pace_ms{0};  // pause between two requests of ONE game (the run's current interval)
  // Cancel: the main thread creates this file, the shell around every curl notices it and kills curl (B must not wait for a download)
  std::string cancel_path() const { return tmp.empty() ? std::string() : tmp + "/cancel"; }
  bool cancelled() const { return !tmp.empty() && ::access(cancel_path().c_str(), F_OK) == 0; }
  void request_cancel() {
    if (tmp.empty()) return;
    int fd = ::open(cancel_path().c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) ::close(fd);
  }
  ~Env() {
    if (!tmp.empty()) {
      ::unlink(cancel_path().c_str());
      ::rmdir(tmp.c_str());
    }
  }
};

struct StepResult {
  enum class Kind : uint8_t { Found, NotFound, Error, Retry, Fatal } kind = Kind::Error;
  Err err = Err::None;
  Abort fatal = Abort::None;
  int wait_ms = 0;
  bool network = false;
  bool warn = false;
  GameFieldEdit edit;
  size_t media = 0;
  uint64_t bytes = 0;
  // connect step
  int interval_ms = 0, quota_left = 0;
};

namespace {

bool write_all_fd(int fd, const char* p, size_t n) {
  while (n) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= size_t(w);
  }
  return true;
}

std::string cfg_quote(std::string_view s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o.push_back('\\');
    if (c == '\n') o += "\\n";
    else o.push_back(c);
  }
  return o + "\"";
}

struct Xfer {
  std::string url, out;
  std::vector<std::string> headers;  // IGDB: "Name: value" lines
  std::string body;                  // IGDB: POST data (carries the client secret: only in the 0600 config file)
  bool post = false;
  int max_time = 40;
  uint64_t max_size = 4u << 20;
};
struct XRes {
  int exitcode = -1, http = 0, retry = 0;
  uint64_t size = 0;
  std::string ctype;
};

// One curl process for all of `xs`. The URLs (with their credentials) live only in a 0600 file of the private temporary
// directory, removed right after. Returns false when curl could not be started at all or `dry` (dry-run executor).
bool run_curl(Env& env, const std::vector<Xfer>& xs, std::vector<XRes>& res, bool& dry) {
  dry = false;
  res.assign(xs.size(), XRes());
  if (env.cancelled()) return false;
  std::string cfg_path = env.tmp + "/c" + std::to_string(++env.seq);
  std::string cfg;
  int total_time = 0;
  for (size_t i = 0; i < xs.size(); ++i) {
    if (i) cfg += "next\n";
    cfg += "url = " + cfg_quote(xs[i].url) + "\n";
    cfg += "output = " + cfg_quote(xs[i].out) + "\n";
    // https stays https, also across redirects; an http URL (ArcadeDB's media links) may move to https: `proto` limits the redirect
    // too, so "=http" alone made curl refuse it (exit 1, shown as "Network error"). Never file:// or ftp://.
    cfg += starts_with(xs[i].url, "https://") ? "proto = \"=https\"\nproto-redir = \"=https\"\n" : "proto = \"=http,https\"\nproto-redir = \"=http,https\"\n";
    for (const std::string& hd : xs[i].headers) cfg += "header = " + cfg_quote(hd) + "\n";
    if (xs[i].post) cfg += "data-raw = " + cfg_quote(xs[i].body) + "\n";
    cfg += "silent\nlocation\nconnect-timeout = 15\nspeed-limit = 1\nspeed-time = 20\nuser-agent = \"zfront\"\n";
    cfg += "max-time = " + std::to_string(xs[i].max_time) + "\nmax-filesize = " + std::to_string(xs[i].max_size) + "\n";
    // a format that starts with @ is a FILE name for curl ("@@..." made every transfer fail with exit 26): a newline first, as ra.cpp
    cfg += "write-out = \"\\n@@%{exitcode}|%{http_code}|%{size_download}|%header{retry-after}|%{content_type}\\n\"\n";
    total_time += xs[i].max_time + 5;
  }
  int fd = ::open(cfg_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  bool ok = write_all_fd(fd, cfg.data(), cfg.size());
  ::close(fd);
  std::fill(cfg.begin(), cfg.end(), '\0');  // do not leave the credentials around in freed memory
  if (!ok) {
    ::unlink(cfg_path.c_str());
    return false;
  }
  // curl in the background, a watcher (no stdout, so it does not hold the pipe) kills it as soon as the cancel file exists
  std::string watch = "(while kill -0 $p 2>/dev/null; do [ -e " + shell_quote(env.cancel_path()) + " ] && kill $p; sleep 1; done) >/dev/null 2>&1 &";
  CmdResult r = env.sv->cmd.run(shell_quote(env.curl) + " -K " + shell_quote(cfg_path) + " 2>/dev/null & p=$!; " + watch + " wait $p", (total_time + 10) * 1000);
  ::unlink(cfg_path.c_str());
  if (r.dry) {
    dry = true;
    return false;
  }
  size_t n = 0, pos = 0;
  while (n < xs.size()) {
    size_t at = r.out.find("@@", pos);
    if (at == std::string::npos) break;
    size_t e = r.out.find('\n', at);
    if (e == std::string::npos) e = r.out.size();
    std::string line = r.out.substr(at + 2, e - at - 2);
    pos = e;
    XRes& x = res[n++];
    std::vector<std::string> f;
    size_t b = 0;
    for (int k = 0; k < 4; ++k) {
      size_t bar = line.find('|', b);
      if (bar == std::string::npos) break;
      f.push_back(line.substr(b, bar - b));
      b = bar + 1;
    }
    if (f.size() == 4) {
      x.ctype = line.substr(b);
      long v = 0;
      x.exitcode = parse_int(f[0], v) ? int(v) : -1;
      x.http = parse_int(f[1], v) ? int(v) : 0;
      x.size = parse_int(f[2], v) && v > 0 ? uint64_t(v) : 0;
      x.retry = parse_int(f[3], v) && v > 0 ? int(std::min<long>(v, 3600)) : 0;
    }
  }
  return true;
}

struct Api {
  bool started = false, dry = false;
  int exitcode = -1, http = 0, retry = 0;
  std::string body;
};

Api api_call(Env& env, const HttpRequest& rq) {
  Api a;
  std::vector<Xfer> xs(1);
  xs[0].url = rq.url;
  xs[0].headers = rq.headers;
  xs[0].body = rq.body;
  xs[0].post = rq.post;
  xs[0].out = env.tmp + "/body";
  xs[0].max_time = env.o.api_timeout_s;
  std::vector<XRes> rs;
  a.started = run_curl(env, xs, rs, a.dry);
  if (!a.started) return a;
  a.exitcode = rs[0].exitcode;
  a.http = rs[0].http;
  a.retry = rs[0].retry;
  if (a.exitcode == 0 && read_file(xs[0].out, a.body) && a.body.size() > (4u << 20)) a.body.resize(4u << 20);
  ::unlink(xs[0].out.c_str());
  return a;
}

Api api_get(Env& env, const std::string& url) {
  HttpRequest r;
  r.url = url;
  return api_call(env, r);
}

// first bytes -> extension of what the file really is ("" = not a media file: a ScreenScraper text such as NOMEDIA/CRCOK)
const char* sniff_ext(const unsigned char* b, size_t n) {
  auto is = [&](size_t off, const char* s) { return n >= off + std::strlen(s) && std::memcmp(b + off, s, std::strlen(s)) == 0; };
  if (n >= 8 && b[0] == 0x89 && is(1, "PNG")) return ".png";
  if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return ".jpg";
  if (is(0, "GIF8")) return ".gif";
  if (is(0, "RIFF") && is(8, "WEBP")) return ".webp";
  if (is(0, "RIFF") && is(8, "AVI ")) return ".avi";
  if (is(4, "ftyp")) return is(8, "qt  ") ? ".mov" : ".mp4";
  if (is(0, "%PDF")) return ".pdf";
  if (n >= 4 && b[0] == 0x1A && b[1] == 0x45 && b[2] == 0xDF && b[3] == 0xA3) return ".webm";
  if (is(0, "BM")) return ".bmp";
  return "";
}

bool mkdir_p(const std::string& dir) {
  if (is_dir(dir)) return true;
  size_t slash = dir.find_last_of('/');
  if (slash != std::string::npos && slash > 0 && !mkdir_p(dir.substr(0, slash))) return false;
  return ::mkdir(dir.c_str(), 0755) == 0 || (errno == EEXIST && is_dir(dir));
}

// curl exit 23 = write error: "no space on the card" only when the folder's disk is nearly full (else a permission or other error)
uint64_t min_free_bytes() {
  long mb = 8;
  if (const char* e = std::getenv("ZF_SCRAPER_MINFREE_MB"); e && *e) parse_int(e, mb);
  return uint64_t(mb < 0 ? 0 : mb) << 20;
}
bool disk_full(const std::string& path) {
  struct statvfs sv {};
  std::string dir = is_dir(path) ? path : path_dir(path);
  if (::statvfs(dir.c_str(), &sv) != 0) return false;
  return uint64_t(sv.f_bavail) * uint64_t(sv.f_frsize) < min_free_bytes();
}

// pause between the requests of one game (one per ScreenScraper system id, one per IGDB platform), like the pause between games
void pace(const Env& env) {
  for (int left = std::min(env.pace_ms.load(), 8000); left > 0 && !env.cancelled(); left -= 50) std::this_thread::sleep_for(std::chrono::milliseconds(std::min(left, 50)));
}

Abort abort_of(ss::Reply r) {
  switch (r) {
    case ss::Reply::BadLogin: return Abort::BadLogin;
    case ss::Reply::ApiClosed: return Abort::ApiClosed;
    case ss::Reply::Maintenance: return Abort::Maintenance;
    case ss::Reply::Blacklisted: return Abort::Blacklisted;
    case ss::Reply::Quota: return Abort::Quota;
    case ss::Reply::TooManyFails: return Abort::TooManyFails;
    default: return Abort::None;
  }
}

StepResult fatal_result(Abort a) {
  StepResult r;
  r.kind = StepResult::Kind::Fatal;
  r.fatal = a;
  return r;
}

// ---- IGDB / ArcadeDB ------------------------------------------------------------------------------------------------
enum class Tok : uint8_t { Ok, NoCreds, Rejected, NoNetwork };

// ES `ensureToken`: Twitch client credentials, kept 30 minutes. Worker thread only (the one FIFO worker owns `env.tok`).
Tok ensure_token(Env& env) {
  if (!env.tok.empty() && Clock::now() < env.tok_until) return Tok::Ok;
  env.tok.clear();
  if (env.o.igdb_id.empty() || env.o.igdb_secret.empty()) return Tok::NoCreds;
  Api a = api_call(env, igdb::token_request(env.o.igdb_id, env.o.igdb_secret));
  if (a.dry || !a.started || a.exitcode != 0 || a.http == 0) return Tok::NoNetwork;
  std::string t;
  long expires = 0;
  if (a.http >= 200 && a.http < 300 && igdb::parse_token(a.body, t, expires)) {
    env.tok = std::move(t);
    env.tok_until = Clock::now() + std::chrono::seconds(igdb::token_ttl_s(expires));
    return Tok::Ok;
  }
  if (a.http >= 500 || a.http == 429) return Tok::NoNetwork;  // Twitch is in trouble: not the user's credentials
  return Tok::Rejected;
}

enum class Hr : uint8_t { Ok, Empty, Net, Timeout, Disk, Busy, Auth, Server };

Hr http_result(const Api& a) {
  if (a.exitcode == 23) return Hr::Disk;
  if (a.exitcode == 28) return Hr::Timeout;
  if (a.exitcode != 0 || a.http == 0) return Hr::Net;
  if (a.http >= 200 && a.http < 300) return Hr::Ok;
  if (a.http == 401 || a.http == 403) return Hr::Auth;
  if (a.http == 429) return Hr::Busy;
  if (a.http == 404 || a.http == 400) return Hr::Empty;
  return Hr::Server;
}

// One game on IGDB or ArcadeDB (searched by the ROM's name, like ES: no hash). True when `found`/`parsed` tell the outcome;
// false with `res` filled when the step ends otherwise (network, busy, fatal).
bool ask_other(Env& env, const Item& it, const SysInfo& sys, int attempt, ss::Parsed& parsed, bool& found, StepResult& res) {
  const Options& o = env.o;
  std::vector<HttpRequest> reqs;  // tried in order until one knows the game
  if (o.source == Source::IGDB) {
    Tok t = ensure_token(env);
    if (t == Tok::NoNetwork) {
      res = fatal_result(Abort::NoNetwork);
      return false;
    }
    if (t != Tok::Ok) {
      res = fatal_result(t == Tok::NoCreds ? Abort::NoCredentials : Abort::InvalidCredentials);
      return false;
    }
    std::string name = search_stem(it.path);
    std::vector<std::string> pids = igdb::platform_ids(sys.platform);
    if (pids.empty()) pids.push_back(std::string());
    for (const std::string& p : pids) reqs.push_back(igdb::game_request(o.igdb_id, env.tok, name, p));
  } else {
    reqs.push_back(adb::game_request(path_filename(it.path)));
  }
  bool first_req = true;
  for (const HttpRequest& rq : reqs) {
    if (!first_req) pace(env);
    first_req = false;
    Api a = api_call(env, rq);
    if (a.dry || !a.started) {
      res = fatal_result(Abort::NoNetwork);
      return false;
    }
    switch (http_result(a)) {
      case Hr::Disk:
        if (disk_full(env.tmp)) {
          res = fatal_result(Abort::DiskFull);
        } else {
          res.kind = StepResult::Kind::Error;
          res.err = Err::Server;
        }
        return false;
      case Hr::Timeout:  // one more try before it counts as "no network"
        if (attempt == 0) {
          res.kind = StepResult::Kind::Retry;
          res.wait_ms = o.busy_retry_ms;
          return false;
        }
        [[fallthrough]];
      case Hr::Net:
        res.kind = StepResult::Kind::Error;
        res.err = Err::Network;
        res.network = true;
        return false;
      case Hr::Busy:
        res.kind = StepResult::Kind::Retry;
        res.wait_ms = std::max(o.busy_retry_ms, a.retry * 1000);
        return false;
      case Hr::Auth:
        if (o.source == Source::IGDB) {  // the token ran out: the retry asks for a new one (and fails for good if the secret is wrong)
          env.tok.clear();
          res.kind = StepResult::Kind::Retry;
          res.wait_ms = 0;
          return false;
        }
        res.kind = StepResult::Kind::Error;
        res.err = Err::Server;
        return false;
      case Hr::Server:
        res.kind = StepResult::Kind::Error;
        res.err = Err::Server;
        return false;
      case Hr::Empty: continue;
      case Hr::Ok: break;
    }
    // a 2xx that is not an answer we understand is a server problem, never "game unknown" (that would stamp the game as scraped)
    if (!(o.source == Source::IGDB ? igdb::valid_answer(a.body) : adb::valid_answer(a.body))) {
      res.kind = StepResult::Kind::Error;
      res.err = Err::Server;
      return false;
    }
    bool ok = o.source == Source::IGDB ? igdb::parse_games(a.body, o.wish, parsed, o.resize_px) : adb::parse_games(a.body, o.wish, o.short_title, parsed);
    if (ok && !parsed.name.empty()) {
      found = true;
      break;
    }
  }
  return true;
}

// "Check the account": ssuserInfos.php (IGDB: the token request; ArcadeDB has no account). Wrong login / closed API / no network show up here, before 2000 games are queued up.
StepResult connect(Env& env, AccountInfo* ai = nullptr) {
  if (env.o.source == Source::ArcadeDB) {
    if (ai) ai->reached = true, ai->id = source_name(env.o.source);
    StepResult r;
    r.kind = StepResult::Kind::Found;
    return r;
  }
  if (env.o.source == Source::IGDB) {
    Tok t = ensure_token(env);
    if (t == Tok::NoNetwork) {
      if (ai) ai->fail = Abort::NoNetwork;
      return fatal_result(Abort::NoNetwork);
    }
    if (ai) ai->reached = true;
    if (t != Tok::Ok) {
      const Abort why = t == Tok::NoCreds ? Abort::NoCredentials : Abort::InvalidCredentials;
      if (ai) ai->fail = why;
      return fatal_result(why);
    }
    if (ai) ai->id = source_name(env.o.source);
    StepResult r;
    r.kind = StepResult::Kind::Found;
    return r;
  }
  Api a = api_get(env, ss::user_url(env.login));
  if (a.dry || !a.started || a.exitcode != 0 || a.http == 0) {
    if (ai) ai->fail = Abort::NoNetwork;
    return fatal_result(Abort::NoNetwork);
  }
  if (ai) ai->reached = true;
  ss::Reply rep = ss::classify(a.http, a.body);
  if (ss::is_fatal(rep)) {
    if (ai) ai->fail = abort_of(rep);
    return fatal_result(abort_of(rep));
  }
  StepResult r;
  r.kind = StepResult::Kind::Found;
  ss::UserInfo ui;
  if (rep == ss::Reply::Ok && ss::parse_user(a.body, ui)) {
    if (ai) ai->id = ui.id, ai->requests_today = ui.requests_today, ai->max_per_day = ui.max_per_day, ai->max_per_min = ui.max_per_min;
    if (env.o.follow_account_rate && ui.max_per_min > 0) r.interval_ms = std::clamp(60000 / ui.max_per_min, 300, 4000);
    if (ui.max_per_day > 0) {
      r.quota_left = std::max(ui.max_per_day - ui.requests_today, 0);
      if (r.quota_left == 0) return fatal_result(Abort::Quota);
    }
  }
  return r;
}

void add_field(std::vector<std::pair<std::string, std::string>>& f, const char* tag, const std::string& v) {
  if (!v.empty()) f.emplace_back(tag, v);
}

// One game: hash, ask, parse, download the media, build the gamelist edit.
StepResult process(Env& env, const Item& it, int attempt) {
  (void)attempt;
  StepResult res;
  const SysInfo& sys = env.sys[it.sys];
  const Options& o = env.o;

  Ident id;
  bool fresh_hash = false;
  ss::Parsed parsed;
  bool found = false;
  if (o.source != Source::ScreenScraper) {  // IGDB / ArcadeDB: searched by name, nothing to hash
    if (!ask_other(env, it, sys, attempt, parsed, found, res)) return res;
  } else {
    // --- identify the ROM
    struct stat st {};
    bool cached = !it.md5.empty() && !it.crc.empty() && ::stat(it.path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    std::string ext = path_ext_lower(it.path);
    if (cached && ext != ".zip" && ext != ".7z" && ext != ".m3u" && uint64_t(st.st_size) <= kMaxHashBytes) {
      id.size = uint64_t(st.st_size);
      id.md5 = it.md5;
      id.crc = it.crc;
      id.hashed = true;
    } else {
      identify(env.sv->cmd, it.path, sys.extract, id);
      fresh_hash = id.hashed && (!id.md5.empty() || !id.crc.empty());
    }

    // --- ask (one request per ScreenScraper system id of the platform, until one knows the game)
    ss::GameQuery q;
    q.rom_name = path_filename(it.path);
    q.md5 = id.md5;
    q.crc = id.crc;
    q.size = id.hashed || id.size ? id.size : 0;
    ss::RomLang rl = ss::rom_lang(q.rom_name, sys.name, sys.platform, o.locale);
    ss::LangPick lp = ss::pick_lang(rl, o.locale);
    std::vector<int> ids = sys.ids;
    if (ids.empty()) ids.push_back(0);
    bool first_id = true;
    for (int sid : ids) {
      if (!first_id) pace(env);  // ScreenScraper counts requests per minute: no burst between the ids of one game
      first_id = false;
      q.system_id = sid;
      Api a = api_get(env, ss::game_url(env.login, q));
      if (a.dry || !a.started) return fatal_result(Abort::NoNetwork);
      if (a.exitcode != 0 || a.http == 0) {
        if (a.exitcode == 23) {
          if (disk_full(env.tmp)) return fatal_result(Abort::DiskFull);
          res.kind = StepResult::Kind::Error;
          res.err = Err::Server;
          return res;
        }
        if (a.exitcode == 28 && attempt == 0) {  // timed out: one more try before it counts as "no network"
          res.kind = StepResult::Kind::Retry;
          res.wait_ms = o.busy_retry_ms;
          return res;
        }
        res.kind = StepResult::Kind::Error;
        res.err = Err::Network;
        res.network = true;
        return res;
      }
      ss::Reply rep = ss::classify(a.http, a.body);
      if (ss::is_fatal(rep)) return fatal_result(abort_of(rep));
      if (rep == ss::Reply::TooFast) {
        res.kind = StepResult::Kind::Retry;
        res.wait_ms = std::max(o.busy_retry_ms, a.retry * 1000);
        return res;
      }
      if (rep == ss::Reply::Transient) {
        res.kind = StepResult::Kind::Error;
        res.err = Err::Server;
        return res;
      }
      if (rep == ss::Reply::NotFound) continue;
      if (!ss::parse_game(a.body, lp, o.wish, parsed)) {  // a 2xx we cannot read is a server problem, not "game unknown"
        res.kind = StepResult::Kind::Error;
        res.err = Err::Server;
        return res;
      }
      if (!parsed.name.empty()) {
        found = true;
        break;
      }
    }
  }

  GameFieldEdit& ed = res.edit;
  ed.rom = it.path;
  ed.scraper = source_name(o.source);
  ed.scrape_date = iso_local(std::time(nullptr));
  if (fresh_hash) {
    add_field(ed.fields, "md5", id.md5);
    add_field(ed.fields, "crc32", id.crc);
  }
  if (!found) {
    res.kind = StepResult::Kind::NotFound;
    return res;
  }
  res.kind = StepResult::Kind::Found;

  // --- text fields (ES keeps an existing name/description when ScrapeNames/ScrapeDescription are off)
  if (o.overwrite_names || !it.has_name) add_field(ed.fields, "name", parsed.name);
  if (o.overwrite_desc || !it.has_desc) add_field(ed.fields, "desc", parsed.desc);
  add_field(ed.fields, "rating", parsed.rating);
  add_field(ed.fields, "releasedate", parsed.date);
  add_field(ed.fields, "developer", parsed.developer);
  add_field(ed.fields, "publisher", parsed.publisher);
  add_field(ed.fields, "genre", parsed.genre);
  add_field(ed.fields, "players", parsed.players);
  add_field(ed.fields, "family", parsed.family);

  // --- media
  struct Dl {
    int kind;
    std::string base, part;  // <rom dir>/<folder>/<stem>-<suffix>
  };
  std::vector<Dl> dls;
  std::vector<Xfer> xs;
  std::string stem = path_stem(it.path);
  for (const ss::Pick& p : parsed.picks) {
    int ki = kind_index(p.kind);
    if (ki < 0) continue;
    const Kind& k = kKinds[ki];
    std::string base = sys.rom_dir + "/" + k.folder + "/" + stem + "-" + k.suffix;
    if (!env.overwrite_media) {
      if (!it.cur[ki].empty()) continue;  // the gamelist already names an existing file: keep it
      if (!p.ext.empty() && file_exists(base + p.ext)) {  // the file is there but the gamelist does not know it
        add_field(ed.fields, k.tag, std::string("./") + k.folder + "/" + stem + "-" + k.suffix + p.ext);
        continue;
      }
    }
    if (!mkdir_p(sys.rom_dir + "/" + k.folder)) {
      res.warn = true;
      continue;
    }
    Xfer x;
    x.url = o.source == Source::ScreenScraper ? ss::media_url(p.url, k.resize ? o.resize_px : 0, k.resize) : p.url;
    x.out = base + ".part";
    x.max_time = ki == 3 ? o.video_timeout_s : o.media_timeout_s;
    x.max_size = 600ull << 20;
    xs.push_back(std::move(x));
    dls.push_back({ki, base, base + ".part"});
  }
  if (!xs.empty()) {
    // videos last: a slow one must not hold the pictures back
    std::vector<size_t> order(xs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return (dls[a].kind == 3) < (dls[b].kind == 3); });
    std::vector<Xfer> xo;
    std::vector<Dl> dlo;
    for (size_t i : order) xo.push_back(xs[i]), dlo.push_back(dls[i]);
    std::vector<XRes> rs;
    bool dry = false;
    bool started = run_curl(env, xo, rs, dry);
    if (!started) {
      res.warn = true;
      for (const Dl& d : dlo) ::unlink(d.part.c_str());
    } else {
      for (size_t i = 0; i < dlo.size(); ++i) {
        const Dl& d = dlo[i];
        const XRes& x = rs[i];
        bool ok = false;
        if (x.exitcode == 23 && disk_full(d.base)) {
          for (const Dl& u : dlo) ::unlink(u.part.c_str());
          return fatal_result(Abort::DiskFull);
        }
        if (x.exitcode == 0 && x.http >= 200 && x.http < 300) {
          unsigned char head[16] = {0};
          size_t n = 0;
          int fd = ::open(d.part.c_str(), O_RDONLY | O_CLOEXEC);
          if (fd >= 0) {
            ssize_t r = ::read(fd, head, sizeof head);
            n = r > 0 ? size_t(r) : 0;
            ::close(fd);
          }
          const char* real = sniff_ext(head, n);
          if (*real) {
            std::string dest = d.base + real;
            if (::rename(d.part.c_str(), dest.c_str()) == 0) {
              ok = true;
              // the file the gamelist named (another extension) is replaced by this one: do not leave it behind
              const std::string& old = it.cur[d.kind];
              if (!old.empty() && old != dest && starts_with(old, sys.rom_dir + "/")) ::unlink(old.c_str());
              ++res.media;
              res.bytes += x.size;
              const Kind& k = kKinds[d.kind];
              add_field(ed.fields, k.tag, std::string("./") + k.folder + "/" + stem + "-" + k.suffix + real);
            }
          }
        } else if (x.exitcode != 0 || x.http == 0 || x.http >= 500 || x.http == 429) {
          res.warn = true;  // a transfer problem (a 404 is simply "no such media")
        }
        if (!ok) ::unlink(d.part.c_str());
      }
    }
  }
  // B pressed during the game: the media may be incomplete, so the game is not stamped as scraped (it is done again next time)
  if (env.cancelled()) return fatal_result(Abort::NoNetwork);
  return res;
}

// ---- queue building ---------------------------------------------------------------------------------------------------
struct Facts {
  std::string name, desc, md5, crc;
  std::string media[kKindCount];
  time_t scraped = 0;
};

time_t parse_iso(const char* s) {
  int y, mo, d, h, mi, se;
  if (!s || std::sscanf(s, "%4d%2d%2dT%2d%2d%2d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  struct tm t {};
  t.tm_year = y - 1900, t.tm_mon = mo - 1, t.tm_mday = d, t.tm_hour = h, t.tm_min = mi, t.tm_sec = se, t.tm_isdst = -1;
  return std::mktime(&t);
}

bool read_facts(int fd, const Game& g, const char* scraper, Facts& f) {
  uint32_t b = g.meta.span_begin, e = g.meta.span_end;
  if (fd < 0 || e <= b || e - b > (1u << 20)) return false;
  std::string buf(e - b, '\0');
  if (::pread(fd, buf.data(), buf.size(), off_t(b)) != ssize_t(buf.size())) return false;
  pugi::xml_document doc;
  if (!doc.load_buffer(buf.data(), buf.size())) return false;
  pugi::xml_node n = doc.child("game");
  if (!n) return false;
  f.name = std::string(trim(n.child("name").text().get()));
  f.desc = std::string(trim(n.child("desc").text().get()));
  f.md5 = std::string(trim(n.child("md5").text().get()));
  f.crc = std::string(trim(n.child("crc32").text().get()));
  for (size_t i = 0; i < kKindCount; ++i) f.media[i] = std::string(trim(n.child(kKinds[i].tag).text().get()));
  for (pugi::xml_node s : n.children("scrap"))
    if (std::strcmp(s.attribute("name").value(), scraper) == 0) f.scraped = parse_iso(s.attribute("date").value());
  return true;
}

std::vector<Item> prep_system(const Env& env, size_t k) {
  const SysInfo& s = env.sys[k];
  const Options& o = env.o;
  std::vector<Game> games = load_games(s.rom_dir, s.exts, s.gamelist);
  int fd = ::open(s.gamelist.c_str(), O_RDONLY | O_CLOEXEC);
  std::vector<Item> out;
  time_t now = std::time(nullptr);
  for (const Game& g : games) {
    if (!o.only_game.empty() && g.path != o.only_game) continue;
    Facts f;
    read_facts(fd, g, source_name(o.source), f);
    Item it;
    it.sys = uint32_t(k);
    it.path = g.path;
    it.name = g.name;
    it.md5 = f.md5;
    it.crc = f.crc;
    it.has_name = !f.name.empty();
    it.has_desc = !f.desc.empty();
    bool missing_any = false, any = false;
    for (size_t i = 0; i < kKindCount; ++i) {
      if (!f.media[i].empty()) {
        std::string p = resolve_rel_path(f.media[i], s.rom_dir);
        if (file_exists(p)) it.cur[i] = p;
      }
      if (!kind_wanted(i, o.wish) || !kind_in_source(o.source, i)) continue;
      if (it.cur[i].empty()) missing_any = true;
      else any = true;
    }
    switch (o.filter) {
      case Options::Filter::All: break;
      case Options::Filter::MissingAny:
        if (!missing_any) continue;
        break;
      case Options::Filter::MissingAll:
        if (any) continue;
        break;
    }
    if (o.skip_days > 0 && f.scraped != 0 && f.scraped > now - time_t(o.skip_days) * 86400) continue;
    out.push_back(std::move(it));
  }
  if (fd >= 0) ::close(fd);
  return out;
}

}  // namespace

// ---- main-thread side --------------------------------------------------------------------------------------------------
static std::shared_ptr<Env> make_env(Ctx& cx, const Options& o) {
  auto env = std::make_shared<Env>();
  env->sv = cx.sv;
  env->login = {ss::dev_login(), ss::soft_name(), o.user, o.pass};
  const char* zc = std::getenv("ZF_CURL");
  env->curl = (zc && *zc && ::access(zc, X_OK) == 0) ? zc : "curl";
  const char* base = std::getenv("ZF_SCRAPER_TMP");
  std::string tmpl = std::string(base && *base ? base : "/tmp") + "/zf-scraper-XXXXXX";
  std::vector<char> t(tmpl.begin(), tmpl.end());
  t.push_back('\0');
  if (::mkdtemp(t.data())) env->tmp = t.data();
  for (size_t si : o.systems) {
    SysInfo s;
    if (si < cx.systems.size()) {
      const System& sy = cx.systems[si];
      s.name = sy.name;
      s.display = sy.fullname.empty() ? sy.name : sy.fullname;
      s.rom_dir = sy.rom_dir;
      s.gamelist = sy.gamelist;
      s.platform = sy.platform;
      s.exts = sy.exts;
      s.ids = ss::system_ids(sy.platform);
      s.extract = extract_archives_for(sy.platform);
    }
    env->sys.push_back(std::move(s));
  }
  env->overwrite_media = o.filter == Options::Filter::All ? o.overwrite_media : false;
  env->o = o;
  return env;
}

std::vector<size_t> supported_systems(const Ctx& cx, Source s) {
  std::vector<size_t> v;
  for (size_t i = 0; i < cx.systems.size(); ++i)
    if (platform_supported(s, cx.systems[i].platform, cx.systems[i].name)) v.push_back(i);
  return v;
}

std::vector<size_t> supported_systems(const Ctx& cx) { return supported_systems(cx, source_of(cx.es)); }

bool system_supported(const Ctx& cx, size_t sys) {
  Source s = source_of(cx.es);
  return sys < cx.systems.size() && available(s) && platform_supported(s, cx.systems[sys].platform, cx.systems[sys].name);
}

void check_account(Ctx& cx, const Options& o, std::function<void(const AccountInfo&)> done) {
  Options oo = o;
  oo.systems.clear();
  auto env = make_env(cx, oo);
  auto out = std::make_shared<AccountInfo>();
  cx.sv->jobs.post(
      [env, out] {
        if (env->tmp.empty()) {
          out->fail = Abort::NoNetwork;
          return;
        }
        connect(*env, out.get());
      },
      [out, done = std::move(done)] { done(*out); });
}

std::shared_ptr<Run> Run::create(Ctx& cx, Options o) {
  std::shared_ptr<Run> r(new Run());
  r->cx_ = &cx;
  r->sv_ = cx.sv;
  r->interval_ms_ = r->base_interval_ms_ = o.min_interval_ms;
  r->env_ = make_env(cx, o);
  r->env_->pace_ms = r->interval_ms_;
  r->o_ = std::move(o);
  r->items_ = std::make_shared<std::vector<Item>>();
  return r;
}

Run::~Run() = default;

void Run::start() {
  if (!o_.background) busy_ = std::make_unique<oscontract::Busy>();
  last_flush_ = Clock::now();
  if (env_->tmp.empty() || o_.systems.empty()) {
    st_.total = 0;
    finalize(Phase::Finished);
    return;
  }
  phase_ = Phase::Preparing;
  post_prepare(0);
}

void Run::post_prepare(size_t k) {
  inflight_ = true;
  sys_name_ = env_->sys[k].display;
  bump();
  auto out = std::make_shared<std::vector<Item>>();
  auto env = env_;
  auto self = shared_from_this();
  sv_->jobs.post([env, k, out] { *out = prep_system(*env, k); },
                 [self, k, out] {
                   self->inflight_ = false;
                   self->sys_done_ = k + 1;
                   for (Item& i : *out) self->items_->push_back(std::move(i));
                   self->st_.total = self->items_->size();
                   self->bump();
                   if (self->cancel_) return self->finalize(Phase::Cancelled);
                   if (k + 1 < self->env_->sys.size()) return self->post_prepare(k + 1);
                   if (self->items_->empty()) return self->finalize(Phase::Finished);
                   self->post_connect();
                 });
}

void Run::post_connect() {
  inflight_ = true;
  phase_ = Phase::Connecting;
  sys_name_.clear();
  bump();
  auto env = env_;
  auto self = shared_from_this();
  auto res = std::make_shared<StepResult>();
  sv_->jobs.post([env, res] { *res = connect(*env); },
                 [self, res] {
                   self->inflight_ = false;
                   if (res->kind == StepResult::Kind::Fatal) {
                     if (self->cancel_ && res->fatal == Abort::NoNetwork) return self->finalize(Phase::Cancelled);  // curl was killed by B
                     self->abort_ = res->fatal;
                     return self->finalize(Phase::Aborted);
                   }
                   if (res->interval_ms > 0) self->interval_ms_ = self->base_interval_ms_ = std::max(self->interval_ms_, res->interval_ms);
                   self->quota_left_ = res->quota_left;
                   if (self->cancel_) return self->finalize(Phase::Cancelled);
                   self->phase_ = Phase::Running;
                   self->due_ = Clock::now();
                   self->bump();
                   self->post_step();
                 });
}

void Run::post_step() {
  if (next_ >= items_->size() || inflight_) return;
  inflight_ = true;
  const Item& it = (*items_)[next_];
  env_->pace_ms = interval_ms_;
  game_name_ = it.name;
  sys_name_ = env_->sys[it.sys].display;
  bump();
  auto env = env_;
  auto items = items_;
  size_t idx = next_;
  int attempt = attempt_;
  auto res = std::make_shared<StepResult>();
  auto self = shared_from_this();
  sv_->jobs.post([env, items, idx, attempt, res] { *res = process(*env, (*items)[idx], attempt); }, [self, res] { self->on_step(*res); });
}

int Run::wait_seconds() const {
  if (phase_ != Phase::Running || inflight_) return 0;
  auto left = std::chrono::duration_cast<std::chrono::milliseconds>(due_ - Clock::now()).count();
  return left > 2000 ? int((left + 999) / 1000) : 0;
}

std::optional<float> Run::wake_in() const {
  if (phase_ != Phase::Running || inflight_ || cancel_ || next_ >= items_->size()) return std::nullopt;
  auto left = std::chrono::duration<float>(due_ - Clock::now()).count();
  return left > 0 ? left : 0.0f;
}

void Run::tick() {
  if (phase_ == Phase::Running && !inflight_ && !cancel_ && next_ < items_->size() && Clock::now() >= due_) post_step();
}

void Run::on_step(StepResult& r) {
  inflight_ = false;
  const Item& it = (*items_)[next_];
  // after B, a step that did not get its answer was cut by B (the watcher kills curl): not an error of that game, nor done
  // (it is scraped next time). Only reached under load: the request outlived the watcher's one-second check.
  if (cancel_ && r.kind != StepResult::Kind::Found && r.kind != StepResult::Kind::NotFound) return finalize(Phase::Cancelled);
  if (r.kind == StepResult::Kind::Fatal) {
    abort_ = r.fatal;
    return finalize(Phase::Aborted);
  }
  if (r.kind == StepResult::Kind::Retry) {
    interval_ms_ = std::min(interval_ms_ * 2, 8000);
    if (++attempt_ > o_.max_retries) {  // give this one up and go on
      errors_.push_back({it.name, Err::Busy, false});
      ++st_.errors;
      ++st_.done;
      ++next_;
      attempt_ = 0;
      bump();
      if (cancel_) return finalize(Phase::Cancelled);
      return after_item();
    }
    due_ = Clock::now() + std::chrono::milliseconds(r.wait_ms);
    bump();
    if (cancel_) return finalize(Phase::Cancelled);
    return;  // tick posts it again when due
  }
  attempt_ = 0;
  if (r.kind != StepResult::Kind::Error && interval_ms_ > base_interval_ms_)  // the server is answering again: ease back to the normal pace
    interval_ms_ = std::max(base_interval_ms_, interval_ms_ - interval_ms_ / 8);
  ++st_.done;
  switch (r.kind) {
    case StepResult::Kind::Found:
      ++st_.found;
      net_fail_streak_ = 0;
      st_.media += r.media;
      if (r.media > 0) ++st_.media_games;
      st_.bytes += r.bytes;
      if (r.warn) {
        ++st_.warnings;
        errors_.push_back({it.name, Err::Network, true});
      }
      break;
    case StepResult::Kind::NotFound:
      ++st_.not_found;
      net_fail_streak_ = 0;
      break;
    default:
      ++st_.errors;
      errors_.push_back({it.name, r.err, false});
      if (r.network) ++net_fail_streak_;
      break;
  }
  if (r.kind == StepResult::Kind::Found || r.kind == StepResult::Kind::NotFound) {
    pending_.push_back(std::move(r.edit));
    pending_sys_.push_back(it.sys);
    if (std::find(touched_.begin(), touched_.end(), it.sys) == touched_.end()) touched_.push_back(it.sys);
    ++since_flush_;
  }
  ++next_;
  bump();
  if (net_fail_streak_ >= 3) {
    abort_ = Abort::NoNetwork;
    return finalize(Phase::Aborted);
  }
  if (cancel_) return finalize(Phase::Cancelled);
  after_item();
}

void Run::after_item() {
  if (next_ >= items_->size()) return finalize(Phase::Finished);
  auto now = Clock::now();
  if (since_flush_ >= size_t(o_.flush_every) || (since_flush_ > 0 && now - last_flush_ >= std::chrono::seconds(o_.flush_secs))) flush(false);
  due_ = now + std::chrono::milliseconds(interval_ms_);
  if (interval_ms_ <= 0) post_step();  // else `tick` posts it when due
}

void Run::cancel() {
  if (finished() || cancel_) return;
  cancel_ = true;
  env_->request_cancel();
  bump();
  if (flushing_) return;
  if (inflight_) {
    phase_ = Phase::Stopping;
    return;
  }
  finalize(Phase::Cancelled);
}

void Run::finalize(Phase end) {
  end_ = end;
  if (phase_ == Phase::Stopping && flushing_) return;
  phase_ = Phase::Stopping;
  bump();
  flush(true);
}

namespace {
struct FlushOut {
  std::string err;
  size_t failed = 0;
  std::vector<std::pair<uint32_t, std::vector<Game>>> fresh;
  bool wrote_ok = true;
};
}  // namespace

void Run::flush(bool last) {
  std::vector<GameFieldEdit> edits = std::move(pending_);
  std::vector<uint32_t> owners = std::move(pending_sys_);
  pending_.clear();
  pending_sys_.clear();
  since_flush_ = 0;
  last_flush_ = Clock::now();
  std::vector<uint32_t> touched = touched_;
  if (!last && edits.empty()) return;
  if (last) flushing_ = true;
  if (last && edits.empty() && touched.empty()) {  // nothing was written: end at once
    flushing_ = false;
    phase_ = end_;
    busy_.reset();
    bump();
    if (!watched_ && end_ == Phase::Cancelled) sv_->toasts.push(tr("SCRAPING STOPPED"));
    return;
  }
  auto env = env_;
  auto out = std::make_shared<FlushOut>();
  auto self = shared_from_this();
  sv_->jobs.post(
      [env, edits = std::move(edits), owners = std::move(owners), touched, last, out] {
        std::unordered_map<uint32_t, std::vector<GameFieldEdit>> by;
        for (size_t i = 0; i < edits.size(); ++i) by[owners[i]].push_back(edits[i]);
        for (auto& kv : by) {
          const SysInfo& s = env->sys[kv.first];
          GamelistBatchResult br;
          std::string err;
          if (!gamelist_edit_fields(s.gamelist, s.rom_dir, kv.second, err, &br)) {
            out->wrote_ok = false;
            out->err = err;
          }
          out->failed += br.failed;
        }
        if (last)
          for (uint32_t k : touched) out->fresh.emplace_back(k, load_games(env->sys[k].rom_dir, env->sys[k].exts, env->sys[k].gamelist));
      },
      [self, out, last] {
        if (!out->wrote_ok) self->errors_.push_back({"gamelist.xml", Err::Write, false});
        if (out->failed) self->errors_.push_back({"gamelist.xml", Err::Write, true});
        for (auto& f : out->fresh) self->apply_fresh(f.first, std::move(f.second));
        if (last) {
          self->flushing_ = false;
          self->phase_ = self->end_;
          self->busy_.reset();
          if (!self->watched_ && self->end_ != Phase::Finished) {
            self->sv_->toasts.push(self->end_ == Phase::Aborted ? describe(self->abort_) : tr("SCRAPING STOPPED"), 4000);
          } else if (!self->watched_) {
            self->sv_->toasts.push(tr("SCRAPING FINISHED"), 3000);
          }
        }
        self->bump();
      });
}

// The lists that are open keep their order and length (a ListScreen indexes into them): only the metadata and names of the
// games that are still there are replaced, matched by path.
void Run::apply_fresh(size_t k, std::vector<Game>&& fresh) {
  if (k >= o_.systems.size() || o_.systems[k] >= cx_->systems.size()) return;
  System& s = cx_->systems[o_.systems[k]];
  if (s.games.empty()) return;  // never opened: it loads the new data when it is
  std::unordered_map<std::string, size_t> at;
  at.reserve(fresh.size());
  for (size_t i = 0; i < fresh.size(); ++i) at[fresh[i].path] = i;
  for (Game& g : s.games) {
    auto it = at.find(g.path);
    if (it == at.end()) continue;
    g.meta = fresh[it->second].meta;
    g.name = fresh[it->second].name;
  }
  games_changed();  // an open list shows the new names
}

}  // namespace zf::scraper
