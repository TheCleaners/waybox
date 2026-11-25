#ifndef WB_MONITOR_LAYOUT_HPP
#define WB_MONITOR_LAYOUT_HPP

/*
 * Monitor arrangement for wbconf's arranger: monitors as boxes at their
 * logical size, placed edge to edge. Pure; unit-tested.
 *
 * A box dropped anywhere ends up touching another (sharing part of an
 * edge, or a corner) and overlapping none: the nearest such place to
 * where it was dropped. Then the whole arrangement is moved to start at
 * 0,0, as positions in rc.xml are.
 */
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wb {

struct LayoutBox {
	std::string name;
	int x = 0, y = 0, w = 0, h = 0;
	bool operator==(const LayoutBox &) const = default;
};

/* A monitor's size on the desktop: its mode over its scale, turned for
 * rotation ("90", "flipped-270"...). At least 1x1. */
std::pair<int, int> logical_size(int mode_w, int mode_h, double scale,
		std::string_view transform);

/* Overlapping (with area); touching (sharing part of an edge, or a
 * corner, without overlapping). */
bool boxes_overlap(const LayoutBox &a, const LayoutBox &b);
bool boxes_touch(const LayoutBox &a, const LayoutBox &b);

/* While dragging box `i` to (x, y): each of its edges pulled onto another
 * box's edges within `threshold`. */
std::pair<int, int> snap_position(const std::vector<LayoutBox> &boxes, size_t i,
		int x, int y, int threshold);

/* Where box `i` goes when dropped at (x, y): there, if it overlaps no box
 * and touches one (or is alone); else the nearest place that does.
 * nullopt if there's none (never, in practice). */
std::optional<std::pair<int, int>> place_box(const std::vector<LayoutBox> &boxes,
		size_t i, int x, int y);

/* Moved so the leftmost and topmost edges are at 0. */
void normalize_layout(std::vector<LayoutBox> &boxes);

/* The line under a monitor's name in the arranger: "make model" (the
 * make once, if the model starts with it too), else its description less
 * a trailing "(NAME)", which the name already says. Control characters
 * become spaces, runs of spaces one, at most `max` bytes (cut at a UTF-8
 * character, with "…"). Expects valid UTF-8. */
std::string monitor_caption(std::string_view make, std::string_view model,
		std::string_view description, std::string_view name, size_t max = 120);

/* "1920,0" -> {1920, 0}; nullopt for anything else. */
std::optional<std::pair<int, int>> parse_position(std::string_view s);

}  // namespace wb

#endif
