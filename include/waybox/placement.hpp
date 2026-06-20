#ifndef WB_PLACEMENT_HPP
#define WB_PLACEMENT_HPP

#include <optional>
#include <span>
#include <string_view>

#include "waybox/geometry.hpp"

namespace wb {

/*
 * Initial window placement policies (where a newly-mapped window goes), mapped
 * onto Openbox's <placement><policy>. All of these are pure functions over
 * rectangles so they can be unit-tested without a compositor; the map handler
 * gathers the existing window rects and the cursor position and calls them.
 */
enum class PlacementPolicy {
	Smart,       /* minimise overlap with existing windows */
	Center,      /* centre of the placement area */
	UnderMouse,  /* centred on the pointer, clamped into the area */
};

std::optional<PlacementPolicy> placement_policy_from_name(std::string_view name);

/*
 * Choose a top-left position for a width x height window inside `area` that
 * minimises overlap with `windows`. Candidate origins are the area's top-left
 * plus the right and bottom edges of existing windows; the top-left-most
 * lowest-overlap candidate that fits in the area wins. Falls back to the area's
 * top-left when nothing fits (window larger than the area).
 */
Rect place_smart(const Rect &area, int width, int height,
		std::span<const Rect> windows);

/* Centre a width x height window within `area`. */
Rect place_center(const Rect &area, int width, int height);

/* Place a width x height window centred on (cursor_x, cursor_y), clamped so it
 * stays within `area`. */
Rect place_under_mouse(const Rect &area, int width, int height,
		int cursor_x, int cursor_y);

}  // namespace wb

#endif /* WB_PLACEMENT_HPP */
