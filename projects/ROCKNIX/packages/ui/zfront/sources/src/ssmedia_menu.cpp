// The rows of SCREENSAVER SETTINGS for the slideshow and the random video (ES GuiGeneralScreensaverOptions::addSlideShowScreensaverOptions /
// addVideoScreensaverOptions, 123-370 of the file; this ES build has no separate classes for them: they are groups of the same screen, shown
// for the current SCREENSAVER TYPE, and the screen is rebuilt when the type or one of the switches that add rows changes).
//
//   ES row                                            key (default)                                   here
//   IMAGE DURATION (SECS) 1..60 / VIDEO DURATION 10..1000   ScreenSaverSwapImageTimeout 10000 / ...VideoTimeout 30000
//   SHOW GAME INFO (switch, slideshow) / (list, video)      SlideshowScreenSaverGameName true / ScreenSaverGameInfo never
//   SHOW DATE TIME (+ DATE FORMAT, TIME FORMAT)             ScreenSaverDateTime false, ScreenSaverDateFormat, ScreenSaverTimeFormat
//   USE MARQUEE AS GAME INFO                                ScreenSaverMarquee true (video: only without custom videos, as ES)
//   DECORATION SET USED                                     ScreenSaverDecorations     hidden: zfront draws no bezels (ES shows it only with the bezel scripts)
//   STRETCH IMAGES / STRETCH VIDEOS                         SlideshowScreenSaverStretch / StretchVideoOnScreenSaver false
//   MUTE VIDEO AUDIO (video)                                ScreenSaverVideoMute false
//   SCREENSAVER CONTROLS                                    ScreenSaverControls        hidden: no launching from the screensaver here (a key only wakes it)
//   USE CUSTOM IMAGES / USE CUSTOM VIDEOS                   SlideshowScreenSaverCustomImageSource / ...VideoSource false
//   CUSTOM IMAGE|VIDEO DIRECTORY                            SlideshowScreenSaverImageDir / ...VideoDir     typed with the keyboard (no file browser yet)
//   USE IMAGES|VIDEOS IN SUBFOLDERS OF CUSTOM DIRECTORY     SlideshowScreenSaverRecurse / ...VideoRecurse false
//   CUSTOM IMAGE|VIDEO FILE EXTENSIONS                      SlideshowScreenSaverImageFilter ".png,.jpg" / ...VideoFilter ".mp4,.avi"
// STOP MUSIC ON SCREENSAVER belongs to the music module.
#include <algorithm>

#include "ssmedia.hpp"

namespace zf {
namespace ssmedia {

namespace {

void save(Ctx& cx) {
  std::string err;
  if (!cx.es.save(&err)) cx.sv->toasts.push(tr("Could not save the settings"));
}

}  // namespace

void build_rows(MenuBuilder& m, Ctx& cx) {
  const std::string* b = cx.es.get("ScreenSaverBehavior");
  const bool video = b && *b == "random video";
  if (!video && !(b && *b == "slideshow")) return;
  Ctx* c = &cx;
  const Refresh again = m.refresh;
  auto rebuilding_toggle = [&m, c, again](const char* label, const char* key, bool def) {
    m.toggle(tr(label), [c, key, def] { return c->es.get_bool(key, def); },
             [c, key, again](bool on) {
               c->es.set_bool(key, on);
               save(*c);
               if (again) again();  // rows below depend on it
             });
  };
  auto plain_toggle = [&m, c](const char* label, const char* key, bool def) {
    m.toggle(tr(label), [c, key, def] { return c->es.get_bool(key, def); }, [c, key](bool on) {
      c->es.set_bool(key, on);
      save(*c);
    });
  };
  auto text_row = [&m, c](const char* label, const char* key, const char* def) {
    m.text(tr(label), [c, key, def] {
      const std::string* v = c->es.get(key);
      return v ? *v : std::string(def);
    }, [c, key](const std::string& v) {
      c->es.set_string(key, v);
      save(*c);
    });
  };
  auto seconds_row = [&m, c](const char* label, const char* key, int lo, int hi, int def_s) {
    m.slider(tr(label), lo, hi, 1, [c, key, def_s, lo, hi] { return int(std::min<long>(std::max<long>(c->es.get_int(key, def_s * 1000L) / 1000, lo), hi)); },
             [c, key](int v) { c->es.set_int(key, long(v) * 1000L); })
        .unit("s")
        .commit([c](int) { save(*c); });
  };

  m.header(video ? tr("RANDOM VIDEO SCREENSAVER SETTINGS") : tr("SLIDESHOW SCREENSAVER SETTINGS"));
  if (video) seconds_row(N_("VIDEO DURATION (SECS)"), "ScreenSaverSwapVideoTimeout", 10, 1000, 30);
  else seconds_row(N_("IMAGE DURATION (SECS)"), "ScreenSaverSwapImageTimeout", 1, 60, 10);

  if (video) {
    m.choice(tr("SHOW GAME INFO"), {{"always", tr("always")}, {"start & end", tr("start & end")}, {"never", tr("never")}},
             [c] {
               const std::string* v = c->es.get("ScreenSaverGameInfo");
               return v && !v->empty() ? *v : std::string("never");
             },
             [c](const std::string& v) {
               c->es.set_string("ScreenSaverGameInfo", v);
               save(*c);
             });
  } else {
    plain_toggle(N_("SHOW GAME INFO"), "SlideshowScreenSaverGameName", true);
  }

  rebuilding_toggle(N_("SHOW DATE TIME"), "ScreenSaverDateTime", false);
  if (cx.es.get_bool("ScreenSaverDateTime", false)) {
    auto fmt = [&m, c](const char* label, const char* key, const char* def, std::vector<Option> opts) {
      m.choice(tr(label), std::move(opts),
               [c, key, def] {
                 const std::string* v = c->es.get(key);
                 return v ? *v : std::string(def);
               },
               [c, key](const std::string& v) {
                 c->es.set_string(key, v);
                 save(*c);
               });
    };
    fmt(N_("DATE FORMAT"), "ScreenSaverDateFormat", "%Y-%m-%d", {{"%Y-%m-%d", "%Y-%m-%d"}, {"%d-%m-%Y", "%d-%m-%Y"}, {"%A, %B %d", "%A, %B %d"}, {"%b %d, %Y", "%b %d, %Y"}});
    fmt(N_("TIME FORMAT"), "ScreenSaverTimeFormat", "%H:%M:%S", {{"%H:%M:%S", "%H:%M:%S"}, {"%I:%M %p", "%I:%M %p"}, {"%p %I:%M", "%p %I:%M"}});
  }

  const bool custom = cx.es.get_bool(video ? "SlideshowScreenSaverCustomVideoSource" : "SlideshowScreenSaverCustomImageSource", false);
  if (!video || !custom) plain_toggle(N_("USE MARQUEE AS GAME INFO"), "ScreenSaverMarquee", true);
  if (video) {
    plain_toggle(N_("STRETCH VIDEOS"), "StretchVideoOnScreenSaver", false);
    plain_toggle(N_("MUTE VIDEO AUDIO"), "ScreenSaverVideoMute", false);
  } else {
    plain_toggle(N_("STRETCH IMAGES"), "SlideshowScreenSaverStretch", false);
  }

  rebuilding_toggle(video ? N_("USE CUSTOM VIDEOS") : N_("USE CUSTOM IMAGES"), video ? "SlideshowScreenSaverCustomVideoSource" : "SlideshowScreenSaverCustomImageSource", false);
  if (custom) {
    if (video) {
      text_row(N_("CUSTOM VIDEO DIRECTORY"), "SlideshowScreenSaverVideoDir", "");
      plain_toggle(N_("USE VIDEOS IN SUBFOLDERS OF CUSTOM DIRECTORY"), "SlideshowScreenSaverVideoRecurse", false);
      text_row(N_("CUSTOM VIDEO FILE EXTENSIONS"), "SlideshowScreenSaverVideoFilter", ".mp4,.avi");
    } else {
      text_row(N_("CUSTOM IMAGE DIRECTORY"), "SlideshowScreenSaverImageDir", kScreenshotsDir);
      plain_toggle(N_("USE IMAGES IN SUBFOLDERS OF CUSTOM DIRECTORY"), "SlideshowScreenSaverRecurse", false);
      text_row(N_("CUSTOM IMAGE FILE EXTENSIONS"), "SlideshowScreenSaverImageFilter", ".png,.jpg");
    }
  }
}

}  // namespace ssmedia
}  // namespace zf
