#include "music.hpp"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>

#include "app.hpp"
#include "menu.hpp"
#include "settings.hpp"
#include "util.hpp"

namespace zf {
namespace music {

// ---- settings ----------------------------------------------------------------------------------------------------------------
Config read_config(const EsSettings& es) {
  Config c;
  c.enabled = es.get_bool("audio.bgmusic", true);                  // Settings.cpp:392
  c.per_system = es.get_bool("audio.persystem", false);            // :393
  c.titles = es.get_bool("audio.display_titles", true);            // :394
  c.theme_music = es.get_bool("audio.thememusics", true);          // :395
  c.title_secs = title_seconds(es.get_int("audio.display_titles_time", 10));  // :396, Window::displayNotificationMessage
  c.volume = es.get_int("MusicVolume", 128);                       // :313
  c.stop_on_saver = es.get_bool("StopMusicOnScreenSaver", false);  // :271
  c.lowers = es.get_bool("VideoLowersMusic", true);                // :309
  c.video_audio = es.get_bool("VideoAudio", true);                // :307
  return c;
}

int max_volume(long linear) {
  if (linear <= 0) return 0;
  const double v = std::pow(10.0, double(linear - 100) / 40.0) * 128.0;  // AudioManager::getMaxMusicVolume
  return v > 128.0 ? 128 : int(v);
}

int ducked_volume(int max) {
  int v = max / 20;
  if (max > 0 && v == 0) v = 1;
  return v;
}

int title_seconds(long v) { return v <= 2 || v > 120 ? 10 : int(v); }

// ---- picking a track ---------------------------------------------------------------------------------------------------------
bool is_audio_file(const std::string& path) {
  static const char* const kExt[] = {".mp3", ".wav", ".ogg", ".flac", ".mod", ".xm", ".stm", ".s3m", ".far", ".it", ".669", ".mtm", ".opus"};
  const std::string e = path_ext_lower(path);
  for (const char* x : kExt)
    if (e == x) return true;
  return false;
}

namespace {

bool is_directory(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void walk(const std::string& dir, bool per_system, const std::string& system, int depth, std::vector<std::string>& out) {
  if (depth > 8) return;
  DIR* d = ::opendir(dir.c_str());
  if (!d) return;
  std::vector<std::string> names;
  while (const dirent* e = ::readdir(d)) {
    const std::string n = e->d_name;
    if (n.empty() || n[0] == '.') continue;  // ".", "..", hidden files (macOS "._x.mp3" copies)
    names.push_back(n);
  }
  ::closedir(d);
  std::sort(names.begin(), names.end());
  for (const std::string& n : names) {
    const std::string p = path_join(dir, n);
    if (is_directory(p)) {
      if (!per_system || n == system) walk(p, per_system, system, depth + 1, out);  // getMusicIn: `anySystem || mSystemName == folder`
    } else if (is_audio_file(p)) {
      out.push_back(p);
    }
  }
}

}  // namespace

void files_in(const std::string& dir, bool per_system, const std::string& system, std::vector<std::string>& out) {
  if (!is_directory(dir)) return;
  walk(dir, per_system, system, 0, out);
}

std::vector<std::string> find_files(const std::vector<std::string>& dirs, bool per_system, const std::string& system) {
  std::vector<std::string> out;
  for (const std::string& d : dirs) {
    files_in(d, per_system, system, out);
    if (!out.empty()) break;  // playRandomMusic: `if (musics.empty()) getMusicIn(next folder)`
  }
  return out;
}

std::vector<std::string> search_dirs(const Config& c, const std::string& theme_dir) {
  std::vector<std::string> v;
  // changePlaylist: the theme's folder only counts when persystem is off and thememusics on
  if (!theme_dir.empty() && c.theme_music && !c.per_system) v.push_back(theme_dir);
  v.push_back("/storage/roms/music");                     // Paths.cpp ROCKNIX mUserMusicPath
  v.push_back("/storage/.config/emulationstation/music");  // Paths::getMusicPath() (= userEmulationStationPath + "/music")
  return v;
}

void Recent::add(const std::string& song, size_t total) {
  const size_t keep = size_t(std::floor(double(total) * 0.4));  // LAST_PLAYED_SIZE
  if (keep < 1) return;                                          // too few tracks to bother with
  while (q_.size() > keep) q_.pop_back();
  q_.push_front(song);
}

bool Recent::has(const std::string& song) const { return std::find(q_.begin(), q_.end(), song) != q_.end(); }

size_t pick_index(const std::vector<std::string>& files, const Recent& recent, const std::function<size_t(size_t)>& rnd) {
  if (files.empty()) return 0;
  size_t i = rnd(files.size());
  long tries = long(files.size()) - 1;
  while (tries > 0 && recent.has(files[i])) {
    i = rnd(files.size());
    --tries;
  }
  return i;
}

// ---- song titles -------------------------------------------------------------------------------------------------------------
namespace {

uint32_t be32(const unsigned char* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint32_t synchsafe(const unsigned char* p) { return (uint32_t(p[0] & 0x7f) << 21) | (uint32_t(p[1] & 0x7f) << 14) | (uint32_t(p[2] & 0x7f) << 7) | (p[3] & 0x7f); }

// An ID3v2 text frame body (encoding byte + text) as UTF-8: the first value only.
std::string decode_text(const unsigned char* p, size_t n) {
  std::string out;
  if (n < 2) return out;
  const unsigned enc = p[0];
  ++p;
  --n;
  if (enc == 0 || enc == 3) {
    size_t len = 0;
    while (len < n && p[len]) ++len;
    if (enc == 3) return std::string(reinterpret_cast<const char*>(p), len);
    for (size_t i = 0; i < len; ++i) utf8_append(out, p[i]);  // ISO-8859-1
    return out;
  }
  bool le = false;
  if (enc == 1 && n >= 2) {  // UTF-16 with a byte order mark
    if (p[0] == 0xFF && p[1] == 0xFE) le = true;
    p += 2;
    n -= 2;
  }
  auto unit = [&](size_t i) -> uint32_t { return le ? uint32_t(p[i] | (p[i + 1] << 8)) : uint32_t((p[i] << 8) | p[i + 1]); };
  for (size_t i = 0; i + 1 < n; i += 2) {
    uint32_t u = unit(i);
    if (u == 0) break;
    if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
      const uint32_t lo = unit(i + 2);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      }
    }
    utf8_append(out, u);
  }
  return out;
}

// "title - artist" from the TIT2 / TPE1 frames of an ID3v2.3 / 2.4 tag; empty when the file has no title frame.
std::string id3v2_title(const std::string& path) {
  std::string h;
  if (!read_file_range(path, 0, 10, h)) return {};
  const unsigned char* b = reinterpret_cast<const unsigned char*>(h.data());
  if (h.size() < 10 || h.compare(0, 3, "ID3") != 0 || (b[3] != 3 && b[3] != 4)) return {};
  const unsigned ver = b[3];
  size_t total = synchsafe(b + 6);
  total = std::min<size_t>(total, 1u << 20);
  std::string body;
  if (total == 0 || !read_file_range(path, 10, total, body)) return {};
  const unsigned char* d = reinterpret_cast<const unsigned char*>(body.data());
  size_t pos = 0;
  if (b[5] & 0x40) {  // extended header: skip it
    if (body.size() < 4) return {};
    const size_t ext = ver == 4 ? synchsafe(d) : size_t(be32(d)) + 4;
    pos = std::min(ext, body.size());
  }
  std::string title, artist;
  while (pos + 10 <= body.size()) {
    if (d[pos] == 0) break;  // padding
    const std::string id(reinterpret_cast<const char*>(d + pos), 4);
    const size_t sz = ver == 4 ? synchsafe(d + pos + 4) : size_t(be32(d + pos + 4));
    pos += 10;
    if (sz > body.size() - pos) break;
    if (id == "TIT2") title = decode_text(d + pos, sz);
    else if (id == "TPE1") artist = decode_text(d + pos, sz);
    pos += sz;
  }
  title = std::string(trim(title));
  if (title.empty()) return {};
  artist = std::string(trim(artist));
  return artist.empty() ? title : title + " - " + artist;
}

std::string fixed_field(const std::string& s) {  // NUL terminated text of a fixed-size field, trimmed
  const size_t z = s.find('\0');
  return std::string(trim(std::string_view(s).substr(0, z)));
}

// ID3v1: the last 128 bytes. ES prints " - title - artist" with the padding; this is the same without the leading dash.
std::string id3v1_title(const std::string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0 || st.st_size < 128) return {};
  std::string t;
  if (!read_file_range(path, uint64_t(st.st_size) - 128, 128, t) || t.compare(0, 3, "TAG") != 0) return {};
  std::string title = fixed_field(t.substr(3, 30));
  std::string artist = fixed_field(t.substr(33, 30));
  if (title.empty()) return {};
  return artist.empty() ? title : title + " - " + artist;
}

}  // namespace

std::string song_title(const std::string& path) {
  const std::string ext = path_ext_lower(path);
  // tracker modules: the title field of the header (AudioManager::playSong)
  struct Mod {
    const char* ext;
    size_t off, len;
  };
  static const Mod kMods[] = {{".mod", 0, 20}, {".stm", 0, 20}, {".s3m", 0, 28}, {".669", 0, 108}, {".mtm", 4, 20}, {".it", 4, 20}, {".far", 4, 40}, {".xm", 17, 20}};
  for (const Mod& m : kMods) {
    if (ext != m.ext) continue;
    std::string t;
    if (read_file_range(path, m.off, m.len, t)) {
      const std::string name = fixed_field(t);
      if (!name.empty()) return name;
    }
    break;
  }
  if (ext == ".mp3") {
    std::string t = id3v2_title(path);
    if (t.empty()) t = id3v1_title(path);
    if (!t.empty()) return t;
  }
  return path_stem(path_filename(path));
}

// ---- SDL_mixer through dlopen ------------------------------------------------------------------------------------------------
namespace {

// The prototypes of SDL_mixer 2.x that are used; Mix_Music stays opaque. MIX_DEFAULT_FORMAT is AUDIO_S16SYS (0x8010 little-endian).
struct MixApi {
  int (*OpenAudio)(int, uint16_t, int, int) = nullptr;
  void (*CloseAudio)() = nullptr;
  void* (*LoadMUS)(const char*) = nullptr;
  void (*FreeMusic)(void*) = nullptr;
  int (*FadeInMusic)(void*, int, int) = nullptr;
  int (*VolumeMusic)(int) = nullptr;
  int (*HaltMusic)() = nullptr;
  void (*HookMusicFinished)(void (*)()) = nullptr;
  const char* (*GetError)() = nullptr;  // SDL_GetError: Mix_GetError is a macro for it
  // SDL itself (already loaded by the device loop): the mixer holds its own reference on the audio subsystem, as ES does in init() /
  // deinit(), so the navigation cues closing theirs (sdlr.cpp AudioOut) never takes the mixer's device with it.
  int (*InitSubSystem)(uint32_t) = nullptr;
  void (*QuitSubSystem)(uint32_t) = nullptr;
};
constexpr uint32_t kSdlInitAudio = 0x10;  // SDL_INIT_AUDIO

std::function<void()>& finished_cb() {
  static std::function<void()> f;
  return f;
}
void finished_trampoline() {  // SDL_mixer's audio thread: no SDL_mixer calls from here
  if (finished_cb()) finished_cb()();
}

class MixBackend : public Backend {
 public:
  explicit MixBackend(void* lib, const MixApi& api) : lib_(lib), api_(api) {}
  ~MixBackend() override {
    close();
    if (lib_) ::dlclose(lib_);
  }
  bool open() override {
    if (opened_) return true;
    if (api_.InitSubSystem && api_.InitSubSystem(kSdlInitAudio) != 0) {
      std::fprintf(stderr, "zfront: music: SDL audio: %s\n", err());
      return false;
    }
    if (api_.OpenAudio(48000, 0x8010, 2, 4096) < 0 && api_.OpenAudio(44100, 0x8010, 2, 4096) < 0) {  // ES: 48 kHz, then 44.1
      std::fprintf(stderr, "zfront: music: cannot open the audio device: %s\n", err());
      if (api_.QuitSubSystem) api_.QuitSubSystem(kSdlInitAudio);
      return false;
    }
    opened_ = true;
    return true;
  }
  void close() override {
    if (!opened_) return;
    stop();
    api_.CloseAudio();
    if (api_.QuitSubSystem) api_.QuitSubSystem(kSdlInitAudio);
    opened_ = false;
    finished_cb() = nullptr;
  }
  bool play(const std::string& path, int fade_ms) override {
    if (!opened_) return false;
    release();  // the hook is cleared first: halting a track would otherwise report it as ended
    cur_ = api_.LoadMUS(path.c_str());
    if (!cur_) {
      std::fprintf(stderr, "zfront: music: %s: %s\n", path.c_str(), err());
      return false;
    }
    if (api_.FadeInMusic(cur_, 1, fade_ms) == -1) {
      release();
      return false;
    }
    api_.HookMusicFinished(&finished_trampoline);
    return true;
  }
  void stop() override { release(); }
  void set_volume(int v) override {
    if (opened_) api_.VolumeMusic(v);
  }
  void set_on_finished(std::function<void()> cb) override { finished_cb() = std::move(cb); }

 private:
  const char* err() const { return api_.GetError ? api_.GetError() : "?"; }
  void release() {
    api_.HookMusicFinished(nullptr);
    if (cur_) {
      api_.HaltMusic();
      api_.FreeMusic(cur_);
      cur_ = nullptr;
    }
  }
  void* lib_;
  MixApi api_;
  bool opened_ = false;
  void* cur_ = nullptr;
};

std::unique_ptr<Backend> make_real() {
  void* lib = ::dlopen("libSDL2_mixer-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
  if (!lib) lib = ::dlopen("libSDL2_mixer.so", RTLD_NOW | RTLD_LOCAL);
  if (!lib) return nullptr;  // no library: no music, silently
  MixApi a;
  auto sym = [lib](const char* n) { return ::dlsym(lib, n); };
  a.OpenAudio = reinterpret_cast<decltype(a.OpenAudio)>(sym("Mix_OpenAudio"));
  a.CloseAudio = reinterpret_cast<decltype(a.CloseAudio)>(sym("Mix_CloseAudio"));
  a.LoadMUS = reinterpret_cast<decltype(a.LoadMUS)>(sym("Mix_LoadMUS"));
  a.FreeMusic = reinterpret_cast<decltype(a.FreeMusic)>(sym("Mix_FreeMusic"));
  a.FadeInMusic = reinterpret_cast<decltype(a.FadeInMusic)>(sym("Mix_FadeInMusic"));
  a.VolumeMusic = reinterpret_cast<decltype(a.VolumeMusic)>(sym("Mix_VolumeMusic"));
  a.HaltMusic = reinterpret_cast<decltype(a.HaltMusic)>(sym("Mix_HaltMusic"));
  a.HookMusicFinished = reinterpret_cast<decltype(a.HookMusicFinished)>(sym("Mix_HookMusicFinished"));
  a.GetError = reinterpret_cast<decltype(a.GetError)>(::dlsym(RTLD_DEFAULT, "SDL_GetError"));
  a.InitSubSystem = reinterpret_cast<decltype(a.InitSubSystem)>(::dlsym(RTLD_DEFAULT, "SDL_InitSubSystem"));
  a.QuitSubSystem = reinterpret_cast<decltype(a.QuitSubSystem)>(::dlsym(RTLD_DEFAULT, "SDL_QuitSubSystem"));
  if (!a.OpenAudio || !a.CloseAudio || !a.LoadMUS || !a.FreeMusic || !a.FadeInMusic || !a.VolumeMusic || !a.HaltMusic || !a.HookMusicFinished) {
    ::dlclose(lib);
    return nullptr;
  }
  return std::make_unique<MixBackend>(lib, a);
}

// ---- the player (main thread) --------------------------------------------------------------------------------------------------
struct State {
  Ctx* cx = nullptr;
  std::unique_ptr<Backend> be;
  bool probed = false;
  bool started = false;      // start() ran: the frontend is up
  bool open = false;         // the device is open (a track plays or is about to)
  bool in_game = false;      // between pause_for_game and resume_after_game
  bool saver_stopped = false;  // the screensaver took the music away
  bool ducked = false;
  std::string system, theme_dir, current, root;
  Recent recent;
  std::atomic<unsigned> gen{0};  // bumped whenever a track starts or the music stops: a late "ended" is ignored
  std::function<size_t(size_t)> rnd;
};
State& S() {
  static State s;
  return s;
}

size_t random_below(size_t n) {
  static std::mt19937 g{std::random_device{}()};
  return std::uniform_int_distribution<size_t>(0, n - 1)(g);
}

Config cfg() { return S().cx ? read_config(S().cx->es) : Config(); }

void apply_volume() {
  State& s = S();
  if (!s.be || !s.open) return;
  const int m = max_volume(cfg().volume);
  s.be->set_volume(s.ducked ? ducked_volume(m) : m);
}

void stop_all() {
  State& s = S();
  s.gen.fetch_add(1);
  if (s.be && s.open) {
    s.be->stop();
    s.be->close();
  }
  s.open = false;
  s.current.clear();
}

void announce(const std::string& path, const Config& c) {
  State& s = S();
  if (!c.titles || !s.cx) return;
  const std::string name = song_title(path);
  if (!name.empty()) s.cx->sv->toasts.push(name, c.title_secs * 1000);  // Window::processSongTitleNotifications
}

void track_ended(unsigned g);

// A new random track (AudioManager::playRandomMusic(false)). The folders are read BEFORE the device is opened: without music files
// nothing is opened and the frontend costs nothing.
bool start_track() {
  State& s = S();
  if (!s.cx || !s.started || s.in_game || s.saver_stopped || !available()) return false;
  const Config c = cfg();
  if (!c.enabled) return false;
  std::vector<std::string> dirs = search_dirs(c, s.theme_dir);
  if (!s.root.empty())
    for (std::string& d : dirs)
      if (!d.empty() && d[0] == '/' && d.compare(0, s.root.size(), s.root) != 0) d = s.root + d;
  std::vector<std::string> files = find_files(dirs, c.per_system, s.system);
  if (files.empty()) {
    stop_all();
    return false;
  }
  const bool was_open = s.open;
  if (!s.open) {
    if (!s.be->open()) return false;
    s.open = true;
    Services* sv = s.cx->sv.get();
    s.be->set_on_finished([sv] {  // audio thread: only posts to the job queue, whose wake-up reaches the loop
      const unsigned g = S().gen.load();
      sv->jobs.post(nullptr, [g] { track_ended(g); });
    });
    apply_volume();
  }
  const auto& rnd = s.rnd ? s.rnd : std::function<size_t(size_t)>(random_below);
  while (!files.empty()) {
    const size_t i = pick_index(files, s.recent, rnd);
    const std::string path = files[i];
    s.gen.fetch_add(1);
    if (s.be->play(path, 1000)) {  // Mix_FadeInMusic(.., 1, 1000)
      s.current = path;
      s.recent.add(path, files.size());
      announce(path, c);
      return true;
    }
    files.erase(files.begin() + long(i));  // ES: the track cannot be played, ask again among the others
  }
  s.current.clear();
  if (!was_open) stop_all();
  return false;
}

void track_ended(unsigned g) {
  State& s = S();
  if (g != s.gen.load() || !s.open) return;  // stopped or replaced meanwhile
  s.current.clear();
  start_track();  // AudioManager::musicEnd_callback -> playRandomMusic(false)
}

}  // namespace

void set_backend(std::unique_ptr<Backend> b) {
  State& s = S();
  stop_all();
  s.be = std::move(b);
  s.probed = true;
}

bool available() { return S().be != nullptr; }

void set_theme_dir(const std::string& dir) { S().theme_dir = dir; }
std::string current_track() { return S().current; }

void set_random_for_tests(std::function<size_t(size_t)> f) { S().rnd = std::move(f); }
void set_root_for_tests(const std::string& root) { S().root = root; }

void reset_for_tests() {
  State& s = S();
  stop_all();
  s.be.reset();
  s.probed = false;
  s.cx = nullptr;
  s.started = s.in_game = s.saver_stopped = s.ducked = false;
  s.system.clear();
  s.theme_dir.clear();
  s.root.clear();
  s.recent = Recent();
  s.rnd = nullptr;
}

void start(Ctx& cx) {
  State& s = S();
  s.cx = &cx;
  s.started = true;
  if (!s.probed) {  // the library is only looked for here, in the device loop: tests and the PC harness never load it
    s.probed = true;
    s.be = make_real();
  }
  if (!available()) return;
  start_track();
}

void shutdown() {
  State& s = S();
  stop_all();
  s.started = false;
  s.cx = nullptr;
  if (s.be) s.be.reset();  // unloads SDL_mixer
  s.probed = false;
}

void pause_for_game() {
  State& s = S();
  if (!s.started) return;
  s.in_game = true;
  stop_all();  // ES: AudioManager::deinit() before the emulator starts
}

void resume_after_game() {
  State& s = S();
  if (!s.in_game) return;
  s.in_game = false;
  start_track();  // ES: changePlaylist(force) / playRandomMusic after the game
}

void on_screensaver(bool on) {
  State& s = S();
  if (!s.started) return;
  if (on) {
    if (cfg().stop_on_saver && s.open) {  // SystemScreenSaver::startScreenSaver
      stop_all();
      s.saver_stopped = true;
    }
  } else if (s.saver_stopped) {  // :255, when the screensaver ends
    s.saver_stopped = false;
    start_track();
  }
}

void on_system(Ctx& cx, long item) {
  State& s = S();
  if (item < 0 || size_t(item) >= cx.items.size()) return;
  const size_t i = cx.system_of(size_t(item));
  if (i >= cx.systems.size()) return;
  const System& sy = cx.systems[i];
  const std::string& name = sy.theme.empty() ? sy.name : sy.theme;  // ThemeData::getSystemThemeFolder
  if (name == s.system) return;
  s.system = name;
  if (s.started && cfg().per_system) start_track();  // changePlaylist: with persystem every system has its own tracks
}

void next() { start_track(); }

void apply_settings(bool enabled_changed) {
  State& s = S();
  if (!s.started) return;
  const Config c = cfg();
  if (enabled_changed) {
    if (c.enabled) start_track();
    else stop_all();
    return;
  }
  if (!c.enabled) {
    stop_all();
    return;
  }
  apply_volume();
}

void duck(bool on) {
  State& s = S();
  const Config c = cfg();
  s.ducked = on && c.lowers && c.video_audio;  // AudioManager::setVideoPlaying
  apply_volume();
}

// ---- menu rows ----------------------------------------------------------------------------------------------------------------
namespace {

void save(Ctx& cx) {
  std::string err;
  if (!cx.es.save(&err)) cx.sv->toasts.push(tr("Could not save the setting"));
}

}  // namespace

void build_menu(MenuBuilder& m, Ctx& cx) {
  // GuiMenu::openSoundSettings, 5792-5812 (MUSIC VOLUME sits in ES's VOLUME group; it is the first row of this one here)
  m.header(tr("MUSIC"));
  m.toggle(tr("FRONTEND MUSIC"), [&cx] { return cx.es.get_bool("audio.bgmusic", true); }, [&cx](bool b) {
    cx.es.set_bool("audio.bgmusic", b);
    save(cx);
    apply_settings(true);
  });
  m.slider(tr("MUSIC VOLUME"), 0, 100, 1, [&cx] { return int(std::min(std::max(cx.es.get_int("MusicVolume", 128), 0L), 100L)); }, [&cx](int v) {
     cx.es.set_int("MusicVolume", v);
     apply_settings(false);  // audible while dragging
   }).unit("%").commit([&cx](int) { save(cx); });
  m.toggle(tr("DISPLAY SONG TITLES"), [&cx] { return cx.es.get_bool("audio.display_titles", true); }, [&cx](bool b) {
    cx.es.set_bool("audio.display_titles", b);
    save(cx);
  });
  m.slider(tr("SONG TITLE DISPLAY DURATION"), 2, 120, 2, [&cx] { return int(std::min(std::max(cx.es.get_int("audio.display_titles_time", 10), 2L), 120L)); },
           [&cx](int v) { cx.es.set_int("audio.display_titles_time", v); })
      .unit("s")
      .commit([&cx](int) { save(cx); });
  m.toggle(tr("ONLY PLAY SYSTEM-SPECIFIC MUSIC FOLDER"), [&cx] { return cx.es.get_bool("audio.persystem", false); }, [&cx](bool b) {
    cx.es.set_bool("audio.persystem", b);
    save(cx);
    next();  // ES: changePlaylist(force) starts a track of the new folder set
  });
  m.toggle(tr("PLAY SYSTEM-SPECIFIC MUSIC"), [&cx] { return cx.es.get_bool("audio.thememusics", true); }, [&cx](bool b) {
    cx.es.set_bool("audio.thememusics", b);
    save(cx);
    next();
  });
  m.toggle(tr("LOWER MUSIC WHEN PLAYING VIDEO"), [&cx] { return cx.es.get_bool("VideoLowersMusic", true); }, [&cx](bool b) {
    cx.es.set_bool("VideoLowersMusic", b);
    save(cx);
  });
}

void build_saver_row(MenuBuilder& m, Ctx& cx) {
  if (!cx.es.get_bool("audio.bgmusic", true)) return;  // GuiGeneralScreensaverOptions.cpp:65
  m.toggle(tr("STOP MUSIC ON SCREENSAVER"), [&cx] { return cx.es.get_bool("StopMusicOnScreenSaver", false); }, [&cx](bool b) {
    cx.es.set_bool("StopMusicOnScreenSaver", b);
    save(cx);
  });
}

}  // namespace music
}  // namespace zf
