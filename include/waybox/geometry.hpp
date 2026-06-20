#ifndef WB_GEOMETRY_HPP
#define WB_GEOMETRY_HPP

namespace wb {

/*
 * Small, wlroots-free geometry primitives for the usable-area / strut model.
 *
 * A Rect is an axis-aligned box (top-left origin). A Strut is a set of per-edge
 * reservations (e.g. the space a panel claims along an output edge). These are
 * deliberately independent of wlr_box so the geometry can be unit-tested in
 * isolation; call sites convert at the boundary.
 */

struct Rect {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

struct Strut {
	int left = 0;
	int top = 0;
	int right = 0;
	int bottom = 0;
};

inline bool operator==(const Rect &a, const Rect &b) {
	return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
inline bool operator!=(const Rect &a, const Rect &b) { return !(a == b); }

inline bool operator==(const Strut &a, const Strut &b) {
	return a.left == b.left && a.top == b.top && a.right == b.right &&
			a.bottom == b.bottom;
}

/* A rect with no area. */
bool empty(const Rect &r);

/* Shrink a rect inward by a strut, clamping the resulting size to >= 0. This is
 * how the usable area is derived from reserved edges (and how margins inset a
 * maximized window). */
Rect apply_strut(const Rect &area, const Strut &strut);

/* The strut that turns `outer` into `inner` (the inverse of apply_strut when
 * `inner` is contained in `outer`). Negative edges are clamped to 0. */
Strut strut_between(const Rect &outer, const Rect &inner);

/*
 * Move `box` so it does not overlap the reserved edges of `outer` — i.e. the
 * edges where `usable` is inset from `outer`. Edges with no reservation
 * (usable flush with outer) are left free, so a window can still be positioned
 * off-screen there. `box`, `outer` and `usable` must share a coordinate space.
 * Only the position is adjusted; the size is preserved.
 */
Rect constrain_to_usable(Rect box, const Rect &outer, const Rect &usable);

}  // namespace wb

#endif /* WB_GEOMETRY_HPP */
