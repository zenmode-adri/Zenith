// What the system carousel shows and how a rebuilt list replaces the current one (hot reload, `App::maybe_reload_systems`).
//
// `Ctx::systems` keeps every system of es_systems.cfg (the menus list them all); `Ctx::items` are the cards of the carousel: ES settings
// HiddenSystems leaves some out and SortSystems=alpha orders them by full name. Everything here is a pure function of its arguments.
#pragma once
#include <string>
#include <vector>

#include "scene.hpp"
#include "settings.hpp"
#include "systems.hpp"

namespace zf {

/// Cards for `systems` as ES shows them: HiddenSystems (`;` list of system names) left out, `SortSystems` = "alpha" sorts by full name
/// (case-insensitive, stable), anything else keeps the es_systems.cfg order (the other ES orderings need data the console's
/// es_systems.cfg does not have). `SystemItem::sys` is set.
std::vector<SystemItem> make_items(const std::vector<System>& systems, const EsSettings& es);

/// Names of the systems the cards show, in card order.
std::vector<std::string> item_names(const std::vector<System>& systems, const std::vector<SystemItem>& items);

/// True when both lists hold the same systems (name and ROM folder, same order) and show the same cards in the same order.
bool same_systems(const std::vector<System>& a, const std::vector<SystemItem>& items_a, const std::vector<System>& b, const std::vector<SystemItem>& items_b);

/// Moves what a freshly loaded `fresh` can reuse out of `old`: the games already read (and the `loaded` state) and the counts already
/// known, for the systems that are the same (name, ROM folder and extensions).
void carry_over(std::vector<System>& old, std::vector<System>& fresh);

/// Card to select in the new list: the one with the name `old_names[old_cursor]`; when it is gone, the nearest one in the old order
/// (the card after it, then the one before, then two after...) that still exists; 0 when nothing is left in common.
long nearest_item(const std::vector<std::string>& old_names, long old_cursor, const std::vector<std::string>& new_names);

}  // namespace zf
