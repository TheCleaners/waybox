/*
 * Pure geometry primitives for the usable-area / strut model. Depends only on
 * the C++ standard library so it can be linked into the standalone unit test.
 */
#include "waybox/geometry.hpp"

#include <algorithm>

namespace wb {

bool empty(const Rect &r) {
	return r.width <= 0 || r.height <= 0;
}

Rect apply_strut(const Rect &area, const Strut &strut) {
	return Rect{
		area.x + strut.left,
		area.y + strut.top,
		std::max(0, area.width - strut.left - strut.right),
		std::max(0, area.height - strut.top - strut.bottom),
	};
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

}  // namespace wb
