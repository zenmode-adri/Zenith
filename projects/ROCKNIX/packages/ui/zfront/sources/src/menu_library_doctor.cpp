// `menu.library_doctor` (LIBRARY > CHECK LIBRARY): a read-only report on the game library. The scan (library_doctor.cpp) runs on the job
// thread when the screen opens and never changes a file; the rows are info rows, except two that open the screens that fix things.
#include <memory>

#include "i18n.hpp"
#include "library_doctor.hpp"
#include "menu.hpp"
#include "menu_game.hpp"
#include "registry.hpp"
#include "util.hpp"

namespace zf::doctor {
namespace {

constexpr size_t kMaxRows = 50;  // per group; the rest is counted

struct State {
  enum Phase { Idle, Loading, Done } phase = Idle;  // Idle counts as loading: the first build comes before on_open
  Report rep;
  std::vector<std::pair<std::string, size_t>> bios;
};

size_t bios_total(const State& st) {
  size_t n = 0;
  for (const auto& b : st.bios) n += b.second;
  return n;
}
size_t no_image_total(const State& st) {
  size_t n = 0;
  for (const Count& c : st.rep.no_image) n += c.n;
  return n;
}

void more(MenuBuilder& m, size_t total) {
  if (total > kMaxRows) m.info(tr("AND MORE"), std::to_string(total - kMaxRows));
}

void build(MenuBuilder& m, const State& st) {
  if (st.phase != State::Done) {
    m.info(tr("PLEASE WAIT"));
    return;
  }
  // one unit all through: BIOS files, broken files, groups of duplicates, games without a picture (not systems for some of them)
  const size_t problems = bios_total(st) + st.rep.broken.size() + st.rep.duplicates.size() + no_image_total(st);
  if (problems == 0) m.info(tr("NO PROBLEMS FOUND"));
  else m.info(tr("PROBLEMS FOUND"), std::to_string(problems));

  if (!st.bios.empty()) {
    m.header(tr("MISSING BIOS"));
    size_t i = 0;
    for (const auto& b : st.bios) {
      if (i++ >= kMaxRows) break;
      m.info(b.first, std::to_string(b.second));
    }
    more(m, st.bios.size());
    m.submenu(tr("MISSING BIOS CHECK"), "menu.game.bios").value(std::to_string(bios_total(st)));
  }
  if (!st.rep.broken.empty()) {
    m.header(tr("POSSIBLY BROKEN FILES"));
    size_t i = 0;
    for (const Problem& p : st.rep.broken) {
      if (i++ >= kMaxRows) break;
      m.info(p.system + ": " + p.file, tr(p.detail.c_str()));
    }
    more(m, st.rep.broken.size());
  }
  if (!st.rep.duplicates.empty()) {
    m.header(tr("DUPLICATES"));
    size_t i = 0;
    for (const Problem& p : st.rep.duplicates) {
      if (i++ >= kMaxRows) break;
      m.info(p.system + ": " + p.file, "x" + p.detail);
    }
    more(m, st.rep.duplicates.size());
  }
  if (!st.rep.no_image.empty()) {
    m.header(tr("GAMES WITHOUT A PICTURE"));
    size_t i = 0;
    for (const Count& c : st.rep.no_image) {
      if (i++ >= kMaxRows) break;
      m.info(c.system, std::to_string(c.n));
    }
    more(m, st.rep.no_image.size());
    m.submenu(tr("SCRAPER"), "menu.scraper").value(std::to_string(no_image_total(st)));
  }
}

std::unique_ptr<Screen> make_doctor() {
  auto st = std::make_shared<State>();
  MenuSpec spec;
  spec.title = tr("CHECK LIBRARY");
  spec.title_key = N_("CHECK LIBRARY");
  spec.build = [st](MenuBuilder& m, Ctx&) { build(m, *st); };
  spec.on_open = [st](Ctx& cx, const Refresh& refresh) {
    if (st->phase != State::Idle) return;
    st->phase = State::Loading;
    std::vector<System> copy = cx.systems;  // the job opens its own copy: cx.systems belongs to the UI thread
    auto raw = std::make_shared<std::string>();
    std::shared_ptr<Services> sv = cx.sv;
    auto work = [st, sv, copy, raw]() mutable {
      if (sv->cmd.dry_run) {  // tests and the preview harness do not run the script; a sample output can stand in (as menu.game.bios)
        read_file(mg::env().rooted("/tmp/rocknix-systems.out"), *raw);
      } else {
        *raw = sv->cmd.run("rocknix-systems", 120000).out;
      }
      st->rep = scan(std::move(copy));
      st->bios = bios_for(mg::bios_problem_counts(*raw), st->rep.with_games);  // only the systems with games
    };
    auto done = [st, refresh] {
      st->phase = State::Done;
      if (refresh) refresh();
    };
    if (cx.sv->cmd.dry_run) {  // deterministic
      work();
      done();
      return;
    }
    cx.sv->jobs.post_slow(work, done);
  };
  return make_menu(std::move(spec));
}

}  // namespace

ZF_REGISTER_SCREEN("menu.library_doctor", [](Ctx&, const std::string&) { return make_doctor(); });

}  // namespace zf::doctor
