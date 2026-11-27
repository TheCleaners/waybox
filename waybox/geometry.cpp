/*
 * Pure geometry primitives for the usable-area / strut model. Depends only on
 * the C++ standard library so it can be linked into the standalone unit test.
 */
#include "waybox/geometry.hpp"

#include <string_view>

#include <optional>

#include <limits>

#include <algorithm>

namespace wb {

bool empty(const Rect &r) {
	return r.width <= 0 || r.height <= 0;
}

namespace {

/* A 64-bit sum or difference, back into int's range. */
int clamp_int(long long v) {
	return static_cast<int>(std::clamp<long long>(v, std::numeric_limits<int>::min(),
			std::numeric_limits<int>::max()));
}

}  // namespace

Rect apply_strut(const Rect &area, const Strut &strut) {
	/* Wide, then clamped: margins as wide as the area (or wider) give an
	 * empty one, not an overflow. */
	long long w = static_cast<long long>(area.width) - strut.left - strut.right;
	long long h = static_cast<long long>(area.height) - strut.top - strut.bottom;
	return Rect{
		clamp_int(static_cast<long long>(area.x) + strut.left),
		clamp_int(static_cast<long long>(area.y) + strut.top),
		clamp_int(std::max(0LL, w)),
		clamp_int(std::max(0LL, h)),
	};
}

std::optional<int> parse_margin(std::string_view text) {
	size_t a = text.find_first_not_of(" \t\n\r");
	size_t b = text.find_last_not_of(" \t\n\r");
	if (a == std::string_view::npos)
		return std::nullopt;
	text = text.substr(a, b - a + 1);
	if (text.empty() || text.size() > 5 ||
			text.find_first_not_of("0123456789") != std::string_view::npos)
		return std::nullopt;
	int v = 0;
	for (char c : text)
		v = v * 10 + (c - '0');
	return v <= kMaxMargin ? std::optional<int>(v) : std::nullopt;
}

Strut strut_between(const Rect &outer, const Rect &inner) {
	return Strut{
		std::max(0, inner.x - outer.x),
		std::max(0, inner.y - outer.y),
		std::max(0, (outer.x + outer.width) - (inner.x + inner.width)),
		std::max(0, (outer.y + outer.height) - (inner.y + inner.height)),
	};
}

Rect constrain_to_usable(Rect box, const Rect &outer, const Rect &usable) {
	const int out_left = outer.x;
	const int out_top = outer.y;
	const int out_right = outer.x + outer.width;
	const int out_bottom = outer.y + outer.height;

	const int us_left = usable.x;
	const int us_top = usable.y;
	const int us_right = usable.x + usable.width;
	const int us_bottom = usable.y + usable.height;

	if (us_top > out_top && box.y < us_top)
		box.y = us_top;                          /* top edge reserved */
	if (us_left > out_left && box.x < us_left)
		box.x = us_left;                         /* left edge reserved */
	if (us_bottom < out_bottom && box.y + box.height > us_bottom)
		box.y = us_bottom - box.height;          /* bottom edge reserved */
	if (us_right < out_right && box.x + box.width > us_right)
		box.x = us_right - box.width;            /* right edge reserved */

	return box;
}

static int clamp_dimension(int value, int min_value, int max_value) {
	const int floor = std::max(1, min_value);
	if (value < floor)
		value = floor;
	if (max_value > 0) {
		/* A max below the floor is nonsensical; never let it shrink below it. */
		const int ceiling = std::max(max_value, floor);
		if (value > ceiling)
			value = ceiling;
	}
	return value;
}

Rect clamp_resize(Rect box, bool resizing_left, bool resizing_top,
		const SizeHints &hints) {
	const int new_width = clamp_dimension(box.width, hints.min_width, hints.max_width);
	const int new_height = clamp_dimension(box.height, hints.min_height, hints.max_height);

	/* When dragging the left/top edge, keep the opposite edge fixed by shifting
	 * the origin to absorb the size change. */
	if (resizing_left)
		box.x += box.width - new_width;
	if (resizing_top)
		box.y += box.height - new_height;
	box.width = new_width;
	box.height = new_height;
	return box;
}

Rect maximize_within(const Rect &restore, const Rect &area, bool horz, bool vert) {
	Rect r = restore;
	if (horz) {
		r.x = area.x;
		r.width = area.width;
	}
	if (vert) {
		r.y = area.y;
		r.height = area.height;
	}
	return r;
}

Rect place_ime_popup(const Rect &cursor, int width, int height, const Rect &output) {
	/* In 64 bits: the cursor rectangle comes from a client, and anything
	 * an int holds adds up without overflowing; the result is on the
	 * output (its top-left, for a popup bigger than the output: it can't
	 * be made smaller from here). */
	long long w = std::max(0, width), h = std::max(0, height);
	long long x = cursor.x;
	long long y = static_cast<long long>(cursor.y) + std::max(0, cursor.height);
	long long bottom = static_cast<long long>(output.y) + output.height;
	long long right = static_cast<long long>(output.x) + output.width;
	if (y + h > bottom && static_cast<long long>(cursor.y) - h >= output.y)
		y = static_cast<long long>(cursor.y) - h;  /* flip above the cursor */
	if (x + w > right)
		x = right - w;
	if (y + h > bottom)
		y = bottom - h;
	if (x < output.x)
		x = output.x;
	if (y < output.y)
		y = output.y;
	return Rect{clamp_int(x), clamp_int(y), width, height};
}

}  // namespace wb
