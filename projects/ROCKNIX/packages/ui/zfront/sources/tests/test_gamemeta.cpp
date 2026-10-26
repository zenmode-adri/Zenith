// Metadata edits after playing / favouriting (gamemeta.*): memory at once, gamelist.xml on the job thread, spans kept
// right, failures reported; GameTime in the gamelist; the launch error toast.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "app.hpp"
#include "check.hpp"
#include "gamemeta.hpp"
#include "launch.hpp"
#include "util.hpp"

using namespace zf;

namespace {

std::string tmpdir(const char* tag) {
  std::string d = "/tmp/zf_test_gamemeta_" + std::string(tag) + "_" + std::to_string(getpid());
  std::string cmd = "rm -rf " + d + " && mkdir -p " + d;
  CHECK(std::system(cmd.c_str()) == 0);
  return d;
}
void spit(const std::string& p, const std::string& s) {
  FILE* f = std::fopen(p.c_str(), "wb");
  CHECK(f != nullptr);
  if (f) {
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
  }
}
std::string slurp(const std::string& p) {
  std::string s;
  read_file(p, s);
  return s;
}

void settle(Ctx& cx) {
  while (!cx.sv->jobs.idle()) {
    cx.sv->jobs.drain();
    usleep(1000);
  }
  cx.sv->jobs.drain();
}

// five games g0..g4.gb whose descriptions are d0..d4 (to prove that the spans still point at the right element)
struct Fixture {
  std::string dir, xml;
  Ctx cx;
  explicit Fixture(const char* tag) {
    dir = tmpdir(tag);
    xml = dir + "/gamelist.xml";
    std::string x = "<?xml version=\"1.0\"?>\n<gameList>\n";
    for (int i = 0; i < 5; ++i) {
      spit(dir + "/g" + std::to_string(i) + ".gb", "x");
      x += "\t<game>\n\t\t<path>./g" + std::to_string(i) + ".gb</path>\n\t\t<name>G" + std::to_string(i) + "</name>\n\t\t<desc>d" + std::to_string(i) + "</desc>\n";
      if (i == 1) x += "\t\t<playcount>2</playcount>\n\t\t<gametime>1000</gametime>\n";
      x += "\t</game>\n";
    }
    x += "</gameList>\n";
    spit(xml, x);
    System s;
    s.name = "gb";
    s.rom_dir = dir;
    s.gamelist = xml;
    s.exts = {".gb"};
    cx.systems.push_back(std::move(s));
    cx.open_system(0);
  }
  ~Fixture() {
    std::string cmd = "rm -rf " + dir;
    if (std::system(cmd.c_str())) {
    }
  }
  Game& game(size_t i) { return cx.systems[0].games[i]; }
  void check_spans() {
    std::string t = slurp(xml);
    for (Game& g : cx.systems[0].games) {
      CHECK_EQ(t.compare(g.meta.span_begin, 5, "<game"), 0);
      CHECK_EQ(t.substr(g.meta.span_end - 7, 7), std::string("</game>"));
      CHECK_EQ(gamelist_details(xml, dir, g.path, g.meta).desc, "d" + g.name.substr(1));
    }
  }
};

void test_gametime_in_gamelist() {
  Fixture f("gt");
  CHECK_EQ(f.game(1).meta.gametime, 1000u);  // read from <gametime>
  CHECK_EQ(f.game(0).meta.gametime, 0u);
  CHECK(sizeof(GameMeta) <= 64);
  std::string err;
  GamelistEdit ed;
  CHECK(gamelist_update_after_play(f.xml, f.dir, f.dir + "/g1.gb", "20261025T101500", err, &ed, 95));
  std::string t = slurp(f.xml);
  CHECK(t.find("<gametime>1095</gametime>") != std::string::npos && t.find("<playcount>3</playcount>") != std::string::npos);
  CHECK_EQ(t.find("<gametime>", t.find("<gametime>") + 1), std::string::npos);  // a single element
  // a game without the field gets one; add_secs 0 leaves it alone
  CHECK(gamelist_update_after_play(f.xml, f.dir, f.dir + "/g0.gb", "20261025T101501", err, nullptr, 7));
  CHECK(slurp(f.xml).find("<gametime>7</gametime>") != std::string::npos);
  CHECK(gamelist_update_after_play(f.xml, f.dir, f.dir + "/g2.gb", "20261025T101502", err));
  CHECK(slurp(f.xml).find("<path>./g2.gb</path>") != std::string::npos);
  size_t n = 0;
  for (size_t p = 0; (p = slurp(f.xml).find("<gametime>", p)) != std::string::npos; ++p) ++n;
  CHECK_EQ(n, size_t(2));
  // a game that is not in the file is created with its time
  CHECK(gamelist_update_after_play(f.xml, f.dir, f.dir + "/new.gb", "20261025T101503", err, nullptr, 3));
  CHECK(slurp(f.xml).find("<gametime>3</gametime>") != std::string::npos);
  // saturates at the int range ES uses
  CHECK(gamelist_update_after_play(f.xml, f.dir, f.dir + "/g1.gb", "20261025T101504", err, nullptr, 4000000000ull));
  CHECK(slurp(f.xml).find("<gametime>2147483647</gametime>") != std::string::npos);
}

void test_played_async() {
  Fixture f("play");
  setenv("TZ", "UTC", 1);
  tzset();
  time_t t = 1792922100;  // 2026-10-25 10:35:00 UTC
  game_played(f.cx, 0, 1, 95, t);
  // memory first, before the job ran
  CHECK_EQ(f.game(1).meta.playcount, 3u);
  CHECK_EQ(f.game(1).meta.gametime, 1095u);
  CHECK_EQ(f.game(1).meta.lastplayed, pack_lastplayed(iso_local(t)));
  CHECK(f.game(1).meta.lastplayed >= 20261025000000ull);
  settle(f.cx);
  std::string x = slurp(f.xml);
  CHECK(x.find("<playcount>3</playcount>") != std::string::npos && x.find("<gametime>1095</gametime>") != std::string::npos);
  CHECK(x.find("<lastplayed>" + iso_local(t) + "</lastplayed>") != std::string::npos);
  f.check_spans();
  // two quick plays of different games queue up and both land
  game_played(f.cx, 0, 0, 30, t + 60);
  game_played(f.cx, 0, 1, 30, t + 120);
  game_played(f.cx, 0, 4, 0, t + 180);  // session of unknown length: no gametime
  settle(f.cx);
  f.check_spans();
  std::vector<Game> back = load_games(f.dir, {".gb"}, f.xml);
  CHECK_EQ(back.size(), size_t(5));
  for (size_t i = 0; i < 5; ++i) {
    CHECK_EQ(back[i].meta.playcount, f.game(i).meta.playcount);
    CHECK_EQ(back[i].meta.gametime, f.game(i).meta.gametime);
    CHECK_EQ(back[i].meta.lastplayed, f.game(i).meta.lastplayed);
  }
  CHECK_EQ(back[1].meta.playcount, 4u);
  CHECK_EQ(back[1].meta.gametime, 1125u);
  CHECK_EQ(back[4].meta.gametime, 0u);
  // out of range: nothing happens
  game_played(f.cx, 0, 99, 5);
  game_played(f.cx, 9, 0, 5);
  CHECK(f.cx.sv->jobs.idle());
  CHECK(f.cx.sv->toasts.items().empty());
}

void test_favorite() {
  Fixture f("fav");
  CHECK(!f.game(2).meta.favorite);
  game_set_favorite(f.cx, 0, 2, true);
  CHECK(f.game(2).meta.favorite);  // at once
  settle(f.cx);
  f.check_spans();
  CHECK(load_games(f.dir, {".gb"}, f.xml)[2].meta.favorite);
  game_set_favorite(f.cx, 0, 2, false);
  game_played(f.cx, 0, 3, 12, 1792922100);
  settle(f.cx);
  f.check_spans();
  CHECK(!load_games(f.dir, {".gb"}, f.xml)[2].meta.favorite);
  CHECK(f.cx.sv->toasts.items().empty());
}

void test_failure() {
  Fixture f("fail");
  spit(f.xml, "<gameList><game><path>./g2.gb</path>");  // truncated: left untouched, the write fails
  game_set_favorite(f.cx, 0, 2, true);
  CHECK(f.game(2).meta.favorite);
  settle(f.cx);
  CHECK(!f.game(2).meta.favorite);  // put back
  CHECK_EQ(f.cx.sv->toasts.items().size(), size_t(1));
  CHECK_EQ(slurp(f.xml), std::string("<gameList><game><path>./g2.gb</path>"));
  game_played(f.cx, 0, 0, 5, 1792922100);
  settle(f.cx);
  CHECK_EQ(f.cx.sv->toasts.items().size(), size_t(2));
}

void test_iso() {
  setenv("TZ", "UTC", 1);
  tzset();
  CHECK_EQ(iso_local(0), std::string("19700101T000000"));
  CHECK_EQ(pack_lastplayed("19700101T000000"), 19700101000000ull);
  CHECK_EQ(pack_lastplayed("garbage"), 0ull);
}

void test_error_toast() {
  launch::Outcome o;
  CHECK(launch::error_toast(o).empty());
  o.message = "THE EMULATOR EXITED UNEXPECTEDLY";
  CHECK_EQ(launch::error_toast(o), std::string("AN ERROR OCCURRED: THE EMULATOR EXITED UNEXPECTEDLY"));
  o.message = "\n  first line of the log  \nsecond line\n";
  CHECK_EQ(launch::error_toast(o), std::string("AN ERROR OCCURRED: first line of the log"));
  o.message = std::string(300, 'x');
  std::string t = launch::error_toast(o);
  CHECK(utf8_len(t) <= 19 + 45 && t.size() > 19 && t.compare(t.size() - 3, 3, "\xE2\x80\xA6") == 0);
  o.message = "UKNOWN ERROR : 210";
  CHECK_EQ(launch::error_toast(o), std::string("AN ERROR OCCURRED: UKNOWN ERROR : 210"));
  set_translator([](const char* s) -> const char* {
    std::string k = s;
    if (k == "AN ERROR OCCURRED") return "HA OCURRIDO UN ERROR";
    if (k == "UNKNOWN EMULATOR") return "EMULADOR DESCONOCIDO";
    return nullptr;
  });
  o.message = "UNKNOWN EMULATOR";
  CHECK_EQ(launch::error_toast(o), std::string("HA OCURRIDO UN ERROR: EMULADOR DESCONOCIDO"));
  set_translator(nullptr);
}

}  // namespace

int main() {
  test_iso();
  test_gametime_in_gamelist();
  test_played_async();
  test_favorite();
  test_failure();
  test_error_toast();
  return zt::done();
}
