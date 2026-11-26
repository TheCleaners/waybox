#ifndef WB_NAME_LIST_HPP
#define WB_NAME_LIST_HPP

/*
 * An ordered list of names, as a list control edits it (Waybar's modules,
 * a program's monitors): added without duplicates, moved, removed; and
 * the suggestions not in it yet. As a page's value: one name a line.
 * Pure; unit-tested.
 */
#include <string>
#include <string_view>
#include <vector>

namespace wb::names {

using List = std::vector<std::string>;

/* "a\nb" <-> {"a", "b"}; blank lines dropped, each name trimmed. */
List from_text(std::string_view text);
std::string to_text(const List &list);

/* Added at the end, trimmed; false (unchanged) if blank, already there,
 * or with a line break in it. */
bool add(List &list, std::string_view name);
/* By `by` places (-1: up), within the list; false if it can't move. */
bool move(List &list, size_t at, int by);
bool remove(List &list, size_t at);

/* The suggestions not in the list yet, in their order. */
List not_in(const List &suggestions, const List &list);

}  // namespace wb::names

#endif
