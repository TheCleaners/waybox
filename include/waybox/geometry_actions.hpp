#ifndef WB_GEOMETRY_ACTIONS_HPP
#define WB_GEOMETRY_ACTIONS_HPP

/*
 * Openbox's geometry actions, as pure functions on a window's frame (its
 * outer rectangle, decorations included) inside the monitor's usable area:
 * MoveResizeTo, MoveRelative, ResizeRelative, MoveToCenter, and the edge
 * actions (MoveToEdge, GrowToEdge, ShrinkToEdge, GrowToFill), which stop at
 * the next edge: another window's, or the area's. Unit-tested
 * (test/geometry_actions_test.cpp); seat.cpp applies them.
 */
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "waybox/geometry.hpp"

namespace wb {

using ActionArgs = std::map<std::string, std::string>;

enum class EdgeDir { North, South, East, West };
/* "north", "up", "south", "down", "east", "right", "west", "left". */
std::optional<EdgeDir> edge_dir_from_name(std::string_view name);

/* Directional focus's eight directions: "north", ..., "northeast",
 * "southwest" (also up, down, left, right). */
enum class Dir8 { North, NorthEast, East, SouthEast, South, SouthWest, West, NorthWest };
std::optional<Dir8> dir8_from_name(std::string_view name);
/* The window to go to from `from` in direction `dir`: of `candidates`, one
 * whose middle lies that way, the nearest (straight ahead counting more
 * than off to the side); nullopt if none. */
std::optional<size_t> directional_target(const Rect &from, const std::vector<Rect> &candidates,
		Dir8 dir);

/* <x> <y> <width> <height>: a number (pixels from the area's left or top,
 * negative from its right or bottom), "center", "N%" (of the area), or
 * "current"; sizes the same, without "center" or negatives. */
Rect move_resize_to(const Rect &frame, const Rect &area, const ActionArgs &args);
/* <x> <y>: pixels to move by. */
Rect move_relative(const Rect &frame, const ActionArgs &args);
/* <left> <right> <top> <bottom>: pixels each edge moves out (negative: in);
 * Openbox's older <x> and <y> are the right and bottom. */
Rect resize_relative(const Rect &frame, const ActionArgs &args);
Rect move_to_center(const Rect &frame, const Rect &area);

/* Moved that way until one of its edges meets the next edge: another
 * window's (one alongside it) or the area's. */
Rect move_to_edge(const Rect &frame, const Rect &area, const std::vector<Rect> &others,
		EdgeDir dir);
/* That side moved out to the next edge: another window's near side, or the
 * area's. */
Rect grow_to_edge(const Rect &frame, const Rect &area, const std::vector<Rect> &others,
		EdgeDir dir);
/* That side moved in to the nearest edge of a window it overlaps; unchanged
 * when none is inside it. */
Rect shrink_to_edge(const Rect &frame, const std::vector<Rect> &others, EdgeDir dir,
		int min_size = 50);
/* Every side out to the next edge, as far as it goes without overlapping. */
Rect grow_to_fill(const Rect &frame, const Rect &area, const std::vector<Rect> &others);

}  // namespace wb

#endif /* WB_GEOMETRY_ACTIONS_HPP */
