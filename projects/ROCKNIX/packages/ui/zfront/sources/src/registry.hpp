// Screen registry: a module adds its screens WITHOUT touching app.cpp / screens.cpp / main.cpp.
//
//   // in my_menu.cpp
//   ZF_REGISTER_SCREEN("menu.sound", [](zf::Ctx& cx, const std::string& arg) { return std::make_unique<SoundMenu>(cx); });
//
// and from any screen: `return Transition::open("menu.sound");` (or `Transition::open("menu.sound", "arg")`).
// `Registry::has(name)` lets a menu hide an entry whose screen is not built yet. Registration happens in static
// initialisers; `zf_core` is an OBJECT library, so no object file is ever dropped by the linker.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace zf {

struct Ctx;
class Screen;
class App;

using ScreenFactory = std::function<std::unique_ptr<Screen>(Ctx&, const std::string& arg)>;
/// Started once, on the device loop only (never in the headless harness), after the `App` exists: an HTTP server,
/// a watcher thread... It runs on the main thread and may spawn threads; those talk to the UI through
/// `app.cx.sv->jobs.post(work, done)` (`done` runs on the main thread).
using ServiceStart = std::function<void(App&)>;

class Registry {
 public:
  static Registry& instance();
  void add(std::string name, ScreenFactory f);
  bool has(std::string_view name) const;
  /// nullptr when `name` is unknown.
  std::unique_ptr<Screen> make(std::string_view name, Ctx& cx, const std::string& arg) const;
  void add_service(std::string name, ServiceStart f);
  void start_services(App& app) const;

 private:
  struct Impl;
  Registry();
  ~Registry();
  Impl* impl_;
};

struct ScreenRegistrar {
  ScreenRegistrar(const char* name, ScreenFactory f) { Registry::instance().add(name, std::move(f)); }
};

struct ServiceRegistrar {
  ServiceRegistrar(const char* name, ServiceStart f) { Registry::instance().add_service(name, std::move(f)); }
};

#define ZF_CAT2(a, b) a##b
#define ZF_CAT(a, b) ZF_CAT2(a, b)
#define ZF_REGISTER_SCREEN(name, factory) static ::zf::ScreenRegistrar ZF_CAT(zf_registrar_, __LINE__)(name, factory)
#define ZF_REGISTER_SERVICE(name, start) static ::zf::ServiceRegistrar ZF_CAT(zf_service_, __LINE__)(name, start)

}  // namespace zf
