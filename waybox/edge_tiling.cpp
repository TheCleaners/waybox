/*
 * Pure edge-tiling geometry. Standard-library only, so it links into the
 * standalone unit test.
 */
#include "waybox/edge_tiling.hpp"

#include <algorithm>

namespace wb {

namespace {

bool contains(const Rect &r, int x, int y) {
	return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

/* Whether a point just across an edge lies on another monitor. */
bool continues(std::span<const Rect> outputs, const Rect &self, int x, int y) {
	for (const Rect &o : outputs) {
		if (o == self)
			continue;
		if (contains(o, x, y))
			return true;
	}
	return false;
}

}  // namespace

TileZone edge_zone(int x, int y, const Rect &output, std::span<const Rect> outputs,
		int range) {
	if (range <= 0 || !contains(output, x, y))
		return TileZone::None;
	const int right = output.x + output.width;
	const int bottom = output.y + output.height;
	const bool left_edge = x < output.x + range &&
			!continues(outputs, output, output.x - 1, y);
	const bool right_edge = x >= right - range &&
			!continues(outputs, output, right, y);
	const bool top_edge = y < output.y + range &&
			!continues(outputs, output, x, output.y - 1);
	const bool bottom_edge = y >= bottom - range &&
			!continues(outputs, output, x, bottom);

	const int corner_h = std::max(range, output.height / 6);
	const int corner_w = std::max(range, output.width / 6);
	const bool upper = y < output.y + corner_h;
	const bool lower = y >= bottom - corner_h;
	const bool leftish = x < output.x + corner_w;
	const bool rightish = x >= right - corner_w;

	if (left_edge)
		return upper ? TileZone::TopLeft : lower ? TileZone::BottomLeft : TileZone::Left;
	if (right_edge)
		return upper ? TileZone::TopRight : lower ? TileZone::BottomRight : TileZone::Right;
	if (top_edge)
		return leftish ? TileZone::TopLeft : rightish ? TileZone::TopRight : TileZone::Top;
	if (bottom_edge)
		return leftish ? TileZone::BottomLeft
				: rightish ? TileZone::BottomRight : TileZone::None;
	return TileZone::None;
}

Rect tile_rect(TileZone zone, const Rect &a) {
	const int lw = a.width / 2, rw = a.width - lw;
	const int th = a.height / 2, bh = a.height - th;
	switch (zone) {
	case TileZone::Left:        return {a.x, a.y, lw, a.height};
	case TileZone::Right:       return {a.x + lw, a.y, rw, a.height};
	case TileZone::Top:         return a;
	case TileZone::TopLeft:     return {a.x, a.y, lw, th};
	case TileZone::TopRight:    return {a.x + lw, a.y, rw, th};
	case TileZone::BottomLeft:  return {a.x, a.y + th, lw, bh};
	case TileZone::BottomRight: return {a.x + lw, a.y + th, rw, bh};
	case TileZone::None:        break;
	}
	return a;
}

std::optional<TileZone> tile_zone_from_name(std::string_view n) {
	if (n == "left") return TileZone::Left;
	if (n == "right") return TileZone::Right;
	if (n == "up") return TileZone::Top;
	if (n == "up-left") return TileZone::TopLeft;
	if (n == "up-right") return TileZone::TopRight;
	if (n == "down-left") return TileZone::BottomLeft;
	if (n == "down-right") return TileZone::BottomRight;
	return std::nullopt;
}

std::string_view tile_zone_name(TileZone zone) {
	switch (zone) {
	case TileZone::Left:        return "left";
	case TileZone::Right:       return "right";
	case TileZone::Top:         return "top";
	case TileZone::TopLeft:     return "top-left";
	case TileZone::TopRight:    return "top-right";
	case TileZone::BottomLeft:  return "bottom-left";
	case TileZone::BottomRight: return "bottom-right";
	case TileZone::None:        break;
	}
	return "none";
}

}  // namespace wb
