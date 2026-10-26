// Helpers of the system / updates menus. See menu_system.hpp.
#include "menu_system.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "oscontract.hpp"
#include "ui_dialogs.hpp"
#include "util.hpp"

namespace zf {
namespace msys {

namespace {

std::string unescape(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char c = s[++i];
      o += c == 'n' ? '\n' : c == 't' ? '\t' : c;
    } else {
      o += s[i];
    }
  }
  return o;
}

}  // namespace

void load_fixture(const std::string& dir) {
  Probe& p = probe();
  std::string text;
  if (!read_file(dir + "/fixture.txt", text)) return;
  p.use_env = true;
  p.fixture_loaded = true;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (line.compare(0, 5, "root ") == 0) {
      p.root = line.substr(5);
    } else if (line.compare(0, 4, "tmp ") == 0) {
      p.tmp = line.substr(4);
    } else if (line.compare(0, 4, "env ") == 0) {
      size_t eq = line.find('=', 4);
      if (eq != std::string::npos) p.env[line.substr(4, eq - 4)] = line.substr(eq + 1);
    } else if (line.compare(0, 4, "cmd\t") == 0) {
      std::vector<std::string> f;
      size_t a = 4;
      for (int k = 0; k < 2; ++k) {
        size_t t = line.find('\t', a);
        if (t == std::string::npos) break;
        f.push_back(line.substr(a, t - a));
        a = t + 1;
      }
      if (f.size() == 2) {
        long rc = 0;
        parse_int(f[1], rc);
        p.canned[f[0]] = {int(rc), unescape(line.substr(a))};
      }
    }
  }
}

Probe& probe() {
  static Probe p;
  static bool tried = false;
  if (!tried) {
    tried = true;  // before the load: `load_fixture` asks for the probe again
    if (const char* d = std::getenv("ZF_SYS_FIXTURE")) load_fixture(d);
  }
  return p;
}

std::string os_path(const std::string& abs) { return probe().root + abs; }

bool exists(const std::string& abs) { return file_exists(os_path(abs)); }

bool have_bin(const char* exe) { return exists(std::string("/usr/bin/") + exe); }

std::string devenv(const char* name) {
  Probe& p = probe();
  if (p.use_env) {
    auto it = p.env.find(name);
    return it == p.env.end() ? std::string() : it->second;
  }
  const char* v = std::getenv(name);
  return v ? v : "";
}

bool devflag(const char* name) { return devenv(name) == "true"; }

std::string read_all(const std::string& abs) {
  std::string out;
  std::ifstream f(os_path(abs), std::ios::binary);
  if (!f) return out;
  char buf[4096];
  while (f.read(buf, sizeof buf) || f.gcount() > 0) {
    out.append(buf, size_t(f.gcount()));
    if (out.size() >= 65536) break;
  }
  return out;
}

std::string read_line(const std::string& abs) {
  std::string t = read_all(abs);
  size_t nl = t.find('\n');
  if (nl != std::string::npos) t.resize(nl);
  return std::string(trim(t));
}

std::vector<std::string> words(const std::string& text) {
  std::vector<std::string> v;
  std::string cur;
  for (char c : text) {
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
      if (!cur.empty()) v.push_back(std::move(cur)), cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) v.push_back(std::move(cur));
  return v;
}

std::vector<std::string> list_dir(const std::string& abs) {
  std::vector<std::string> v;
  if (DIR* d = ::opendir(os_path(abs).c_str())) {
    while (dirent* e = ::readdir(d)) {
      std::string n = e->d_name;
      if (n != "." && n != "..") v.push_back(n);
    }
    ::closedir(d);
  }
  std::sort(v.begin(), v.end());
  return v;
}

CmdResult query(Services& sv, const std::string& cmd, int timeout_ms) {
  CmdResult r = sv.cmd.run(cmd, timeout_ms);
  if (r.dry) {
    auto it = probe().canned.find(cmd);
    if (it != probe().canned.end()) {
      r.exit_code = it->second.rc;
      r.out = it->second.out;
    }
  }
  return r;
}

void put(Ctx& cx, const std::string& key, const std::string& value, bool keep_default) {
  bool drop = !keep_default && (value.empty() || value == "auto" || value == "default");
  bool ok = drop ? cx.conf.persist_remove(key) : cx.conf.persist(key, value);
  if (!ok) {
    // keep the screen coherent even if the file cannot be written (the toast tells the user)
    if (drop)
      cx.conf.remove(key);
    else
      cx.conf.set(key, value);
    cx.sv->toasts.push(tr("Could not save the setting"));
  }
}

void put_bool(Ctx& cx, const std::string& key, bool v) { put(cx, key, v ? "1" : "0", true); }

std::string get(Ctx& cx, const std::string& key, const std::string& def) { return cx.conf.get_or(key, def); }

bool get_bool(Ctx& cx, const std::string& key, bool def) { return cx.conf.get_bool(key, def); }

bool sysfs_write(Ctx& cx, const std::string& abs, const std::string& text) {
  if (cx.sv->cmd.dry_run) {
    cx.sv->cmd.log.push_back("sysfs: " + abs + " <- " + text);
    return true;
  }
  std::ofstream f(os_path(abs));
  if (!f) return false;
  f << text << "\n";
  return bool(f);
}

void do_reboot(Ctx& cx) {
  Probe& p = probe();
  if (p.reboot) {
    p.reboot(cx);
    return;
  }
  oscontract::quit(oscontract::Mode::Reboot);
}

Transition reboot_prompt(const std::string& message) {
  ui::ConfirmOpts o;
  o.yes = tr("Reboot now");
  o.no = tr("Later");
  return ui::confirm(
      message,
      [](Ctx& cx) {
        do_reboot(cx);
        return Transition::none();
      },
      o);
}

// The confirm dialog cuts what does not fit (it ends in "..."), and ES's warnings are long: the question must never be
// the part that is lost. Below 700 px the blank lines between paragraphs go; below 400 px (480x320) the question moves
// to the front, so what is cut is the tail of the warning.
std::string fit_message(const std::string& text) {
  std::vector<std::string> par;
  size_t a = 0;
  for (;;) {
    size_t b = text.find("\n\n", a);
    if (b == std::string::npos) {
      par.push_back(text.substr(a));
      break;
    }
    par.push_back(text.substr(a, b - a));
    a = b + 2;
  }
  std::string out;
  if (text.size() > 140 && par.size() > 1) {
    out = par.back();
    par.pop_back();
  }
  for (const std::string& p : par) out += (out.empty() ? "" : "\n") + p;
  return out;
}

void post(Ctx& cx, std::function<void()> work, std::function<void()> done) {
  if (probe().fixture_loaded) {
    if (work) work();
    if (done) done();
    return;
  }
  cx.sv->jobs.post(std::move(work), std::move(done));
}

std::vector<Option> options_or_current(const LazyList& l, const std::string& current) {
  if (l.loaded && !l.opts.empty()) return l.opts;
  std::vector<Option> v;
  v.push_back({current, current.empty() ? std::string("...") : current});
  return v;
}

}  // namespace msys
}  // namespace zf
