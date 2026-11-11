// The libvlc engine of `video`. No headers: the few prototypes of libvlc 3.0 (libvlc.so.5 in the image) are declared here and found with
// dlsym; an older / newer ABI that lacks one of them just makes `usable()` false. ES does the same calls (VideoVlcComponent.cpp
// 652-672 libvlc_new, 780-830 startVideo / onMediaParsed): libvlc_new("--quiet", "--no-video-title-show" + system.cfg `vlc.commandline`),
// media from the path (+ `vlc.options`), RGBA frames through lock / unlock / display callbacks, the size shrunk to the box, audio muted
// unless wanted. Difference: ES parses the media first to learn the size; here `libvlc_video_set_format_callbacks` gives it to us when
// the first picture is about to be made (no parse step to wait for).
#include <dlfcn.h>

#include <cstring>
#include <type_traits>
#include <string>
#include <vector>

#include "video.hpp"

namespace zf {
namespace video {
namespace {

struct libvlc_instance_t;
struct libvlc_media_t;
struct libvlc_media_player_t;

using LockCb = void* (*)(void* opaque, void** planes);
using UnlockCb = void (*)(void* opaque, void* picture, void* const* planes);
using DisplayCb = void (*)(void* opaque, void* picture);
using FormatCb = unsigned (*)(void** opaque, char* chroma, unsigned* width, unsigned* height, unsigned* pitches, unsigned* lines);
using CleanupCb = void (*)(void* opaque);

struct Api {
  libvlc_instance_t* (*libvlc_new)(int, const char* const*) = nullptr;
  void (*libvlc_release)(libvlc_instance_t*) = nullptr;
  libvlc_media_t* (*libvlc_media_new_path)(libvlc_instance_t*, const char*) = nullptr;
  void (*libvlc_media_add_option)(libvlc_media_t*, const char*) = nullptr;
  void (*libvlc_media_release)(libvlc_media_t*) = nullptr;
  libvlc_media_player_t* (*libvlc_media_player_new_from_media)(libvlc_media_t*) = nullptr;
  void (*libvlc_media_player_release)(libvlc_media_player_t*) = nullptr;
  int (*libvlc_media_player_play)(libvlc_media_player_t*) = nullptr;
  void (*libvlc_media_player_stop)(libvlc_media_player_t*) = nullptr;
  int (*libvlc_media_player_get_state)(libvlc_media_player_t*) = nullptr;  // libvlc_state_t: 3 Playing, 6 Ended, 7 Error
  void (*libvlc_video_set_callbacks)(libvlc_media_player_t*, LockCb, UnlockCb, DisplayCb, void*) = nullptr;
  void (*libvlc_video_set_format_callbacks)(libvlc_media_player_t*, FormatCb, CleanupCb) = nullptr;
};

constexpr int kStateEnded = 6, kStateError = 7;  // libvlc_state_t (unchanged since libvlc 1.x; ES compares libvlc_Ended)

// What the VLC callbacks need: the store and the box. One per player; freed after `libvlc_media_player_release` (no callback can run then).
struct Cb {
  FrameStore* store = nullptr;
  uint32_t box_w = 0, box_h = 0;
  bool cover = false;
};

unsigned on_format(void** opaque, char* chroma, unsigned* width, unsigned* height, unsigned* pitches, unsigned* lines) {
  Cb* c = static_cast<Cb*>(*opaque);
  std::memcpy(chroma, "RGBA", 4);
  uint32_t ow = 0, oh = 0;
  fit_dims(*width, *height, c->box_w, c->box_h, c->cover, ow, oh);
  *width = ow;
  *height = oh;
  pitches[0] = ow * 4;
  lines[0] = oh;
  c->store->configure(ow, oh);
  return 1;
}
void on_cleanup(void*) {}
void* on_lock(void* opaque, void** planes) {
  Cb* c = static_cast<Cb*>(opaque);
  planes[0] = c->store->begin_write();
  return nullptr;
}
void on_unlock(void*, void*, void* const*) {}
void on_display(void* opaque, void*) { static_cast<Cb*>(opaque)->store->publish(); }

std::vector<std::string> split_words(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && s[i] == ' ') ++i;
    size_t j = i;
    while (j < s.size() && s[j] != ' ') ++j;
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

class VlcEngine : public Engine {
 public:
  ~VlcEngine() override {
    stop();
    release();
  }

  bool usable() override {
    if (tried_) return ok_;
    tried_ = true;
    lib_ = dlopen("libvlc.so.5", RTLD_NOW | RTLD_LOCAL);
    if (!lib_) return false;
    bool all = true;
    auto sym = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib_, name));
      if (!fn) all = false;
    };
    sym(api_.libvlc_new, "libvlc_new");
    sym(api_.libvlc_release, "libvlc_release");
    sym(api_.libvlc_media_new_path, "libvlc_media_new_path");
    sym(api_.libvlc_media_add_option, "libvlc_media_add_option");
    sym(api_.libvlc_media_release, "libvlc_media_release");
    sym(api_.libvlc_media_player_new_from_media, "libvlc_media_player_new_from_media");
    sym(api_.libvlc_media_player_release, "libvlc_media_player_release");
    sym(api_.libvlc_media_player_play, "libvlc_media_player_play");
    sym(api_.libvlc_media_player_stop, "libvlc_media_player_stop");
    sym(api_.libvlc_media_player_get_state, "libvlc_media_player_get_state");
    sym(api_.libvlc_video_set_callbacks, "libvlc_video_set_callbacks");
    sym(api_.libvlc_video_set_format_callbacks, "libvlc_video_set_format_callbacks");
    if (!all) {
      dlclose(lib_);
      lib_ = nullptr;
      return false;
    }
    ok_ = true;
    return true;
  }

  bool start(const Request& r, FrameStore& store) override {
    if (!ok_) return false;
    stop();
    if (!inst_) {
      std::vector<std::string> args = {"--quiet", "--no-video-title-show"};
      for (const std::string& w : split_words(r.vlc_cmdline)) args.push_back(w);
      std::vector<const char*> argv;
      for (const std::string& a : args) argv.push_back(a.c_str());
      inst_ = api_.libvlc_new(int(argv.size()), argv.data());
      if (!inst_) return false;
    }
    libvlc_media_t* media = api_.libvlc_media_new_path(inst_, r.path.c_str());
    if (!media) return false;
    if (r.mute) api_.libvlc_media_add_option(media, ":no-audio");  // no audio stream opened at all
    for (const std::string& w : split_words(r.vlc_options)) api_.libvlc_media_add_option(media, w.c_str());
    mp_ = api_.libvlc_media_player_new_from_media(media);
    api_.libvlc_media_release(media);  // the player keeps its own reference
    if (!mp_) return false;
    cb_ = new Cb{&store, r.box_w, r.box_h, r.cover};
    api_.libvlc_video_set_callbacks(mp_, on_lock, on_unlock, on_display, cb_);
    api_.libvlc_video_set_format_callbacks(mp_, on_format, on_cleanup);
    if (api_.libvlc_media_player_play(mp_) != 0) {
      stop();
      return false;
    }
    return true;
  }

  State state() override {
    if (!mp_) return State::Error;
    const int s = api_.libvlc_media_player_get_state(mp_);
    if (s == kStateEnded) return State::Ended;
    if (s == kStateError) return State::Error;
    return State::Playing;
  }

  bool restart() override {
    if (!mp_) return false;
    api_.libvlc_media_player_stop(mp_);
    return api_.libvlc_media_player_play(mp_) == 0;
  }

  void stop() override {
    if (mp_) {
      api_.libvlc_media_player_stop(mp_);  // 3.x: returns when the decoder and output threads are done
      api_.libvlc_media_player_release(mp_);
      mp_ = nullptr;
    }
    delete cb_;
    cb_ = nullptr;
  }

  void release() override {
    stop();
    if (inst_) {
      api_.libvlc_release(inst_);
      inst_ = nullptr;
    }
    // the library itself stays mapped (dlclose of libvlc is not safe with its plugins' threads); `release` gives back the instance
  }

 private:
  void* lib_ = nullptr;
  bool tried_ = false, ok_ = false;
  Api api_;
  libvlc_instance_t* inst_ = nullptr;
  libvlc_media_player_t* mp_ = nullptr;
  Cb* cb_ = nullptr;
};

}  // namespace

std::unique_ptr<Engine> make_vlc_engine() { return std::make_unique<VlcEngine>(); }

}  // namespace video
}  // namespace zf
