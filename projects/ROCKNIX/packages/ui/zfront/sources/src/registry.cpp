#include "registry.hpp"

#include "app.hpp"

#include <map>
#include <vector>

namespace zf {

struct Registry::Impl {
  std::map<std::string, ScreenFactory, std::less<>> m;
  std::vector<std::pair<std::string, ServiceStart>> services;
};

Registry::Registry() : impl_(new Impl) {}
Registry::~Registry() { delete impl_; }

Registry& Registry::instance() {
  static Registry r;  // constructed on first use: safe from other static initialisers
  return r;
}

void Registry::add(std::string name, ScreenFactory f) { impl_->m[std::move(name)] = std::move(f); }

void Registry::add_service(std::string name, ServiceStart f) { impl_->services.emplace_back(std::move(name), std::move(f)); }

void Registry::start_services(App& app) const {
  for (auto& s : impl_->services) s.second(app);
}

bool Registry::has(std::string_view name) const { return impl_->m.find(name) != impl_->m.end(); }

std::unique_ptr<Screen> Registry::make(std::string_view name, Ctx& cx, const std::string& arg) const {
  auto it = impl_->m.find(name);
  if (it == impl_->m.end()) return nullptr;
  return it->second(cx, arg);
}

}  // namespace zf
