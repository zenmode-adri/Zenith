#include <cstdio>
#include <string>
#include <unistd.h>

#include "check.hpp"
#include "settings.hpp"
#include "util.hpp"

using namespace zf;

int main() {
  {
    // a line break in a key or value must not forge another entry
    SystemConf e = SystemConf::from_text("k=1\n", "");
    e.set("evil", "x\nroot_injected=1");
    e.set("k\nk2", "v\r\nk3=w");
    SystemConf r = SystemConf::from_text(e.text(), "");
    CHECK(r.get("root_injected") == nullptr);
    CHECK(r.get("k2") == nullptr && r.get("k3") == nullptr);
    CHECK(r.get("evil") != nullptr && *r.get("evil") == "x root_injected=1");
    CHECK(r.get("k k2") != nullptr);
    e.remove("k\nk2");
    CHECK(SystemConf::from_text(e.text(), "").get("k k2") == nullptr);
  }
  SystemConf c = SystemConf::from_text("# comment\nsound.volume=70\ngbc.core=mgba\n; other\nwifi.enabled=1\n");
  CHECK(*c.get("gbc.core") == "mgba");
  CHECK(c.get("missing") == nullptr);
  CHECK(c.get_or("missing", "x") == "x");
  CHECK(c.get_bool("wifi.enabled", false));
  c.set("gbc.core", "gambatte");
  c.set("new.key", "v");
  c.remove("sound.volume");
  std::string t = c.text();
  CHECK(t.find("# comment") != std::string::npos);  // untouched lines survive
  CHECK(t.find("gbc.core=gambatte") != std::string::npos);
  CHECK(t.find("new.key=v") != std::string::npos);
  CHECK(t.find("sound.volume") == std::string::npos);

  std::string path = "/tmp/zf_test_settings_" + std::to_string(getpid()) + ".cfg";
  std::string err;
  CHECK(write_atomic(path, "a=1\n", &err));
  std::string back;
  CHECK(read_file(path, back) && back == "a=1\n");
  SystemConf d = SystemConf::from_text("k=1\n", path);
  d.set("k", "2");
  CHECK(d.save(&err));
  CHECK(read_file(path, back) && back.find("k=2") != std::string::npos);
  std::remove(path.c_str());

  EsSettings s = EsSettings::from_text("<?xml version=\"1.0\"?>\n<bool name=\"ShowHidden\" value=\"true\"/>\n<int name=\"ScreenSaverTime\" value=\"300000\"/>\n<string name=\"ThemeSet\" value=\"zenith\"/>\n");
  CHECK(s.get_bool("ShowHidden", false));
  CHECK_EQ(s.get_int("ScreenSaverTime", 0), 300000L);
  CHECK(*s.get("ThemeSet") == "zenith");
  s.set_bool("ShowHidden", false);
  s.set_string("Extra", "e");
  EsSettings s2 = EsSettings::from_text(s.text());
  CHECK(!s2.get_bool("ShowHidden", true));
  CHECK(*s2.get("Extra") == "e");

  // the settings generation moves on every successful save (the idle-time launch preparation watches it)
  {
    std::string p2 = "/tmp/zf_test_settings_gen_" + std::to_string(getpid()) + ".cfg";
    CHECK(write_atomic(p2, "k=1\n"));
    SystemConf g = SystemConf::load(p2);
    unsigned g0 = settings_generation();
    CHECK(g.persist("k", "2"));
    CHECK(settings_generation() != g0);
    g0 = settings_generation();
    EsSettings es = EsSettings::from_text("", p2 + ".es");
    es.set_bool("x", true);
    CHECK(es.save());
    CHECK(settings_generation() != g0);
    std::remove((p2 + ".es").c_str());

    // preview / dry-run: nothing is written, the in-memory state still follows
    set_writes_blocked(true);
    g0 = settings_generation();
    CHECK(g.persist("k", "3"));
    CHECK(g.persist("other", "x"));
    CHECK(g.persist_remove("gone"));
    CHECK_EQ(*g.get("k"), std::string("3"));
    CHECK_EQ(*g.get("other"), std::string("x"));  // the first persist is not lost by the second
    es.set_bool("y", true);
    CHECK(es.save());
    CHECK(write_atomic(p2 + ".new", "z\n"));
    CHECK(!file_exists(p2 + ".new"));
    CHECK_EQ(settings_generation(), g0);
    set_writes_blocked(false);
    std::string disk;
    CHECK(read_file(p2, disk));
    CHECK_EQ(disk, std::string("k=2\n"));
    std::remove(p2.c_str());
  }
  return zt::done();
}
