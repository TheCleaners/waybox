#ifndef WB_EDGE_TILING_HPP
#define WB_EDGE_TILING_HPP

#include <optional>
#include <span>
#include <string_view>

#include "waybox/geometry.hpp"

namespace wb {

/*
 * Edge tiling: drag a window to a monitor edge to tile it there (left or
 * right half, a quarter in a corner, maximized at the top), or use the
 * SnapToEdge action. This is the pure geometry; the live part (preview,
 * applying the tile) is in cursor.cpp and xdg_shell.cpp.
 */
enum class TileZone { None, Left, Right, Top, TopLeft, TopRight, BottomLeft, BottomRight };

/* The zone the pointer at (x, y) is in on `output` (layout coordinates):
 * within `range` pixels of an edge. Near the ends of an edge (a sixth of its
 * length) the zone is the corner quarter. Only outer edges count: an edge
 * that another monitor in `outputs` continues across is not a zone, so
 * dragging from one monitor to the next doesn't tile. */
TileZone edge_zone(int x, int y, const Rect &output, std::span<const Rect> outputs,
		int range);

/* The rectangle a zone covers within `area` (the usable area). Top is the
 * whole area (maximize). */
Rect tile_rect(TileZone zone, const Rect &area);

/* SnapToEdge's <direction>: left, right, up, up-left, up-right, down-left,
 * down-right. ("down", which restores, is handled by the caller.) */
std::optional<TileZone> tile_zone_from_name(std::string_view name);

/* A short name for logs: "left", "top-right", ... */
std::string_view tile_zone_name(TileZone zone);

}  // namespace wb

#endif /* WB_EDGE_TILING_HPP */
