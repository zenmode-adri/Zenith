// ES's PLAYER ASSIGNMENTS (players.hpp): the order the pads get at launch, what the rows show, what they store.
#include <string>
#include <vector>

#include "check.hpp"
#include "launch.hpp"
#include "pad.hpp"
#include "players.hpp"
#include "settings.hpp"

using namespace zf;

namespace {

JoyInfo pad(uint32_t index, const char* guid, const char* name, const char* path = "") {
  JoyInfo j;
  j.index = index;
  j.guid = guid;
  j.name = name;
  j.path = path;
  return j;
}

std::vector<uint32_t> idx(const std::vector<JoyInfo>& v) {
  std::vector<uint32_t> o;
  for (const JoyInfo& j : v) o.push_back(j.index);
  return o;
}

}  // namespace

int main() {
  const std::vector<JoyInfo> joys = {pad(0, "g0", "Built-in"), pad(1, "g1", "Xbox"), pad(2, "g2", "8BitDo")};
  const players::Slots none{};

  // nothing chosen: connection order, as before
  CHECK(idx(players::order(joys, none)) == (std::vector<uint32_t>{0, 1, 2}));
  CHECK(players::order({}, none).empty());
  // free pads: SDL index order whatever their paths say (event10 would sort before event2 as text)
  CHECK(idx(players::order({pad(5, "a", "A", "/dev/input/event10"), pad(3, "b", "B", "/dev/input/event2")}, none)) == (std::vector<uint32_t>{3, 5}));

  {  // name + GUID: the Xbox is player 1
    players::Slots s{};
    s[0] = {"Xbox", "g1", ""};
    CHECK(idx(players::order(joys, s)) == (std::vector<uint32_t>{1, 0, 2}));
    // same name, other GUID: the name alone still finds it (lower priority)
    s[0] = {"Xbox", "other", ""};
    CHECK(idx(players::order(joys, s)) == (std::vector<uint32_t>{1, 0, 2}));
    // a pad that is gone: the player takes a free one
    s[0] = {"Missing", "x", ""};
    CHECK(idx(players::order(joys, s)) == (std::vector<uint32_t>{0, 1, 2}));
  }
  {  // path has the highest priority, over a name that matches another pad
    std::vector<JoyInfo> j = {pad(0, "g", "Same", "/dev/input/event1"), pad(1, "g", "Same", "/dev/input/event2")};
    players::Slots s{};
    s[0] = {"Same", "g", "/dev/input/event2"};
    CHECK(idx(players::order(j, s)) == (std::vector<uint32_t>{1, 0}));
    s[1] = {"Same", "g", "/dev/input/event2"};  // two players with one pad: the first keeps it
    CHECK(idx(players::order(j, s)) == (std::vector<uint32_t>{1, 0}));
  }
  {  // player 3 chose the 8BitDo, player 1 and 2 nothing: free pads first, in order
    players::Slots s{};
    s[2] = {"8BitDo", "g2", ""};
    CHECK(idx(players::order(joys, s)) == (std::vector<uint32_t>{0, 1, 2}));
  }
  {  // holes (ES: the highest player moves down first): only P3 and P4 have a pad of their own and nothing else is connected
    std::vector<JoyInfo> j = {pad(0, "a", "A"), pad(1, "b", "B")};
    players::Slots s{};
    s[2] = {"B", "b", ""};
    s[3] = {"A", "a", ""};
    CHECK(idx(players::order(j, s)) == (std::vector<uint32_t>{0, 1}));  // P4's pad (A) fills P1, then P3's (B) fills P2
  }
  {  // an empty name never matches a pad that has none
    std::vector<JoyInfo> j = {pad(0, "", ""), pad(1, "g", "N")};
    players::Slots s{};
    s[1] = {"N", "g", ""};
    CHECK(idx(players::order(j, s)) == (std::vector<uint32_t>{0, 1}) || idx(players::order(j, s)) == (std::vector<uint32_t>{1, 0}));
    CHECK(players::chosen(j, s)[0] == -1);
  }

  // what the rows show
  {
    players::Slots s{};
    s[0] = {"Xbox", "g1", ""};
    s[1] = {"8BitDo", "g2", "/sys/devices/not-seen-by-sdl"};  // a path from ES that SDL's path never equals: found by name + GUID
    std::vector<int> c = players::chosen(joys, s);
    CHECK_EQ(c.size(), players::kSlots);
    CHECK_EQ(c[0], 1);
    CHECK_EQ(c[1], 2);
    CHECK_EQ(c[2], -1);
    s[2] = {"Xbox", "g1", ""};  // already taken by player 1
    CHECK_EQ(players::chosen(joys, s)[2], -1);
  }

  // what they store: es_settings.cfg strings, as ES
  {
    EsSettings es = EsSettings::from_text("<config></config>", "/tmp/zf_test_players_never_saved.cfg");
    JoyInfo x = pad(1, "g1", "Xbox", "/dev/input/event4");
    CHECK(players::assign(es, 0, &x));
    CHECK(es.get("INPUT P1NAME") && *es.get("INPUT P1NAME") == "Xbox");
    CHECK(es.get("INPUT P1GUID") && *es.get("INPUT P1GUID") == "g1");
    CHECK(es.get("INPUT P1PATH") && *es.get("INPUT P1PATH") == "/dev/input/event4");
    CHECK(!players::assign(es, 0, &x));  // nothing changed
    players::Slots s = players::read(es);
    CHECK_EQ(s[0].name, std::string("Xbox"));
    CHECK(players::assign(es, 0, nullptr));  // back to default
    CHECK(es.get("INPUT P1NAME") && *es.get("INPUT P1NAME") == "DEFAULT");
    CHECK(es.get("INPUT P1GUID") && es.get("INPUT P1GUID")->empty());
    CHECK(es.get("INPUT P1PATH") && es.get("INPUT P1PATH")->empty());
    CHECK(!players::assign(es, 99, &x));
  }

  // the launch honours it: controllers_config orders the pads by the table of the process
  {
    players::set(none);
    CHECK_EQ(launch::controllers_config(joys), std::string(" -p1index 0 -p1guid g0  -p2index 1 -p2guid g1  -p3index 2 -p3guid g2 "));
    players::Slots s{};
    s[0] = {"8BitDo", "g2", ""};
    players::set(s);
    CHECK_EQ(launch::controllers_config(joys), std::string(" -p1index 2 -p1guid g2  -p2index 0 -p2guid g0  -p3index 1 -p3guid g1 "));
    players::set(none);
  }
  return zt::done();
}
