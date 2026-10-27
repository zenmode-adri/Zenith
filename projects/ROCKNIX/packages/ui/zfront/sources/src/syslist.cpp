#include "syslist.hpp"

#include <algorithm>

#include "collections.hpp"
#include "util.hpp"

namespace zf {

std::vector<SystemItem> make_items(const std::vector<System>& systems, const EsSettings& es) {
  const std::vector<std::string> hidden = collections::hidden_systems(es);
  std::vector<size_t> order;
  for (size_t i = 0; i < systems.size(); ++i)
    if (std::find(hidden.begin(), hidden.end(), systems[i].name) == hidden.end()) order.push_back(i);
  const std::string* sort = es.get("SortSystems");
  if (sort && *sort == "alpha") {
    auto key = [&](size_t i) { return utf8_upper(systems[i].fullname.empty() ? systems[i].name : systems[i].fullname); };
    std::vector<std::string> keys(systems.size());
    for (size_t i : order) keys[i] = key(i);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return keys[a] < keys[b]; });
  }
  std::vector<SystemItem> items;
  items.reserve(order.size());
  for (size_t i : order) items.push_back(SystemItem{systems[i].theme, systems[i].fullname, uint32_t(systems[i].count), i});
  return items;
}

std::vector<std::string> item_names(const std::vector<System>& systems, const std::vector<SystemItem>& items) {
  std::vector<std::string> v;
  v.reserve(items.size());
  for (size_t k = 0; k < items.size(); ++k) {
    const size_t s = items[k].sys == kItemIsSystem ? k : items[k].sys;
    v.push_back(s < systems.size() ? systems[s].name : std::string());
  }
  return v;
}

bool same_systems(const std::vector<System>& a, const std::vector<SystemItem>& items_a, const std::vector<System>& b, const std::vector<SystemItem>& items_b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].name != b[i].name || a[i].rom_dir != b[i].rom_dir) return false;
  return item_names(a, items_a) == item_names(b, items_b);
}

void carry_over(std::vector<System>& old, std::vector<System>& fresh) {
  for (System& n : fresh) {
    for (System& o : old) {
      if (o.name != n.name || o.rom_dir != n.rom_dir || o.exts != n.exts) continue;
      if (o.loaded || !o.games.empty()) {
        n.games = std::move(o.games);
        n.loaded = o.loaded;
        n.count = o.count;
      } else if (o.count != kCountPending) {
        n.count = o.count;
      }
      break;
    }
  }
}

long nearest_item(const std::vector<std::string>& old_names, long old_cursor, const std::vector<std::string>& new_names) {
  if (new_names.empty() || old_cursor < 0 || old_cursor >= long(old_names.size())) return 0;
  const long n = long(old_names.size());
  auto find = [&](const std::string& name) -> long {
    auto it = std::find(new_names.begin(), new_names.end(), name);
    return it == new_names.end() ? -1 : long(it - new_names.begin());
  };
  long at = find(old_names[size_t(old_cursor)]);
  if (at >= 0) return at;
  for (long k = 1; k < n; ++k) {  // after first, then before, at growing distance (the carousel wraps)
    at = find(old_names[size_t((old_cursor + k) % n)]);
    if (at >= 0) return at;
    at = find(old_names[size_t(((old_cursor - k) % n + n) % n)]);
    if (at >= 0) return at;
  }
  return 0;
}

}  // namespace zf
