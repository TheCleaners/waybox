/*
 * Pure window-placement policies. Depends only on the C++ standard library and
 * the geometry primitives, so it links into the standalone unit test.
 */
#include "waybox/placement.hpp"

#include <algorithm>
#include <vector>

namespace wb {

namespace {

/* Area of the overlap between two rects (0 if disjoint). Uses long to avoid
 * overflow when summing over many large windows. */
long overlap_area(const Rect &a, const Rect &b) {
	const int ix = std::max(a.x, b.x);
	const int iy = std::max(a.y, b.y);
	const int ax = std::min(a.x + a.width, b.x + b.width);
	const int ay = std::min(a.y + a.height, b.y + b.height);
	const int w = ax - ix;
	const int h = ay - iy;
	if (w <= 0 || h <= 0)
		return 0;
	return static_cast<long>(w) * static_cast<long>(h);
}

bool fits(const Rect &area, int x, int y, int width, int height) {
	return x >= area.x && y >= area.y &&
			x + width <= area.x + area.width &&
			y + height <= area.y + area.height;
}

std::vector<int> sorted_unique(std::vector<int> values) {
	std::sort(values.begin(), values.end());
	values.erase(std::unique(values.begin(), values.end()), values.end());
	return values;
}

}  // namespace

std::optional<PlacementPolicy> placement_policy_from_name(std::string_view name) {
	if (name == "Smart")
		return PlacementPolicy::Smart;
	if (name == "Center" || name == "Centre")
		return PlacementPolicy::Center;
	if (name == "UnderMouse")
		return PlacementPolicy::UnderMouse;
	return std::nullopt;
}

Rect place_smart(const Rect &area, int width, int height,
		std::span<const Rect> windows) {
	/* Candidate origins: the area's top-left, and the points just past each
	 * existing window's right and bottom edges (so windows pack next to each
	 * other). */
	std::vector<int> xs{area.x};
	std::vector<int> ys{area.y};
	for (const Rect &w : windows) {
		xs.push_back(w.x + w.width);
		ys.push_back(w.y + w.height);
	}
	xs = sorted_unique(std::move(xs));
	ys = sorted_unique(std::move(ys));

	bool found = false;
	Rect best{area.x, area.y, width, height};
	long best_overlap = 0;

	/* Iterating y then x ascending gives a top-left bias: ties keep the first
	 * (upper-left-most) candidate because we only replace on strictly lower
	 * overlap. */
	for (int y : ys) {
		for (int x : xs) {
			if (!fits(area, x, y, width, height))
				continue;
			Rect candidate{x, y, width, height};
			long overlap = 0;
			for (const Rect &w : windows)
				overlap += overlap_area(candidate, w);
			if (!found || overlap < best_overlap) {
				found = true;
				best = candidate;
				best_overlap = overlap;
				if (overlap == 0)
					return best;  /* a perfect, non-overlapping spot */
			}
		}
	}

	/* No non-overlapping spot exists (the windows are too large to tile).
	 * Rather than stack every window at the same min-overlap point, cascade
	 * them by a fixed step keyed on the number of existing windows, wrapped to
	 * stay within the area. */
	constexpr int kCascadeStep = 32;
	const int x_span = std::max(1, area.width - width + 1);
	const int y_span = std::max(1, area.height - height + 1);
	const int k = static_cast<int>(windows.size());
	return Rect{
		area.x + (k * kCascadeStep) % x_span,
		area.y + (k * kCascadeStep) % y_span,
		width,
		height,
	};
}

Rect place_center(const Rect &area, int width, int height) {
	return Rect{
		area.x + (area.width - width) / 2,
		area.y + (area.height - height) / 2,
		width,
		height,
	};
}

Rect place_under_mouse(const Rect &area, int width, int height,
		int cursor_x, int cursor_y) {
	int x = cursor_x - width / 2;
	int y = cursor_y - height / 2;
	/* Clamp into the area, but never below its origin even if the window is
	 * larger than the area (which would invert the clamp range). */
	const int max_x = std::max(area.x, area.x + area.width - width);
	const int max_y = std::max(area.y, area.y + area.height - height);
	x = std::clamp(x, area.x, max_x);
	y = std::clamp(y, area.y, max_y);
	return Rect{x, y, width, height};
}

}  // namespace wb
