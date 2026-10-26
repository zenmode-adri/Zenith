// Stage-0 extension points: ConfLock + SystemConf::persist, CmdRunner, Toasts, Jobs, Registry, tr().
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include "app.hpp"
#include "check.hpp"
#include "registry.hpp"
#include "services.hpp"
#include "settings.hpp"

using namespace zf;

static std::string slurp(const std::string& p) {
  std::ifstream f(p);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

namespace {
struct DummyScreen : Screen {
  Transition handle(Action, Ctx&) override { return Transition::none(); }
  void draw(Ctx&, std::vector<Op>&) override {}
};
}  // namespace

ZF_REGISTER_SCREEN("test.dummy", [](Ctx&, const std::string& arg) { (void)arg; return std::make_unique<DummyScreen>(); });

int main() {
  char dir[] = "/tmp/zf-sv-XXXXXX";
  CHECK(::mkdtemp(dir) != nullptr);
  std::string d = dir;

  // ---- lock + persist
  SystemConf::lock_path() = d + "/lock";
  {
    ConfLock a(d + "/lock", 100);
    CHECK(a.ok());
    ConfLock b(d + "/lock", 80);  // held by us: times out
    CHECK(!b.ok());
  }
  {
    ConfLock c(d + "/lock", 100);  // released by the destructor
    CHECK(c.ok());
  }
  {  // a lock of a dead pid is stolen
    std::ofstream f(d + "/lock");
    f << "2000000000\n";
  }
  {
    ConfLock s(d + "/lock", 200);
    CHECK(s.ok());
  }
  std::string cfg = d + "/system.cfg";
  {
    std::ofstream f(cfg);
    f << "audio.volume=50\n# keep me\nwifi.enabled=0\n";
  }
  SystemConf sc = SystemConf::load(cfg);
  CHECK(sc.persist("wifi.enabled", "1"));
  {  // another program edits a different key meanwhile; our next persist must not lose it
    std::ofstream f(cfg, std::ios::app);
    f << "other.key=7\n";
  }
  CHECK(sc.persist("audio.volume", "80"));
  std::string t = slurp(cfg);
  CHECK(t.find("audio.volume=80") != std::string::npos);
  CHECK(t.find("wifi.enabled=1") != std::string::npos);
  CHECK(t.find("other.key=7") != std::string::npos);
  CHECK(t.find("# keep me") != std::string::npos);
  CHECK(sc.persist_remove("other.key"));
  CHECK(slurp(cfg).find("other.key") == std::string::npos);
  CHECK_EQ(sc.get_or("audio.volume", ""), std::string("80"));
  {  // a held lock makes persist fail instead of racing
    ConfLock held(d + "/lock", 100);
    CHECK(!sc.persist("x", "1"));
  }

  // ---- commands
  CmdRunner r;
  CmdResult o = r.run("echo hello; echo err 1>&2; exit 3");
  CHECK_EQ(o.exit_code, 3);
  CHECK(o.out.find("hello") != std::string::npos && o.out.find("err") != std::string::npos);
  CmdResult to = r.run("sleep 5", 150);
  CHECK_EQ(to.exit_code, -2);
  r.dry_run = true;
  CmdResult dr = r.run("rm -rf /");
  CHECK(dr.dry && dr.exit_code == 0 && r.log.size() == 1 && r.log[0] == "rm -rf /");
  CHECK_EQ(shell_quote("a'b c"), std::string("'a'\\''b c'"));
  r.dry_run = false;
  r.spawn("touch " + d + "/spawned");
  for (int i = 0; i < 50 && access((d + "/spawned").c_str(), F_OK) != 0; ++i) ::usleep(20000);
  CHECK(access((d + "/spawned").c_str(), F_OK) == 0);

  // ---- toasts
  Toasts ts;
  ts.push("a", 20);
  ts.push("b", 5000);
  CHECK(ts.items().size() == 2);
  CHECK(ts.next_deadline().has_value());
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  CHECK(ts.expire(std::chrono::steady_clock::now()));
  CHECK(ts.items().size() == 1 && ts.items()[0].text == "b");

  // ---- jobs: FIFO, `done` only inside drain() on this thread
  Jobs jb;
  std::vector<int> order;
  std::atomic<int> woke{0};
  jb.set_wake([&] { ++woke; });
  auto me = std::this_thread::get_id();
  bool main_thread_done = true;
  for (int i = 0; i < 3; ++i)
    jb.post([] { ::usleep(5000); }, [&, i] {
      order.push_back(i);
      if (std::this_thread::get_id() != me) main_thread_done = false;
    });
  for (int i = 0; i < 200 && !jb.idle(); ++i) ::usleep(5000);
  CHECK(order.empty());  // not run yet
  CHECK(jb.drain());
  CHECK(order.size() == 3 && order[0] == 0 && order[1] == 1 && order[2] == 2 && main_thread_done);
  CHECK(woke.load() >= 3);
  CHECK(!jb.drain());

  // ---- registry + Transition::open
  CHECK(Registry::instance().has("test.dummy"));
  CHECK(!Registry::instance().has("test.nope"));
  Ctx cx;
  cx.sv->cmd.dry_run = true;
  App app(std::move(cx), std::make_unique<DummyScreen>(), make_status_source("/nonexistent"));
  auto before = app.cx.sv->toasts.items().size();
  CHECK(Registry::instance().make("test.dummy", app.cx, "x") != nullptr);
  (void)before;

  // ---- tr
  CHECK_EQ(std::string(tr("HELLO")), std::string("HELLO"));
  set_translator([](const char* s) -> const char* { return std::string(s) == "HELLO" ? "HOLA" : nullptr; });
  CHECK_EQ(std::string(tr("HELLO")), std::string("HOLA"));
  CHECK_EQ(std::string(tr("BYE")), std::string("BYE"));
  set_translator(nullptr);

  std::string rm = "rm -rf " + d;
  if (std::system(rm.c_str()) != 0) {
  }
  return zt::done();
}
