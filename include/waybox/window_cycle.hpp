#ifndef WB_WINDOW_CYCLE_HPP
#define WB_WINDOW_CYCLE_HPP

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

#include "waybox/style.hpp"  /* SwitcherOrder */

namespace wb {

/*
 * Pure window-cycle selection (the kernel of Alt+Tab).
 *
 * Given `count` windows in some fixed order, the index of the currently
 * focused window (`current`, or std::nullopt when nothing is focused) and a
 * direction, return the index of the next *eligible* window, scanning in the
 * chosen direction and wrapping around. `eligible(i)` reports whether window i
 * can receive focus (e.g. mapped and not skipped).
 *
 * The currently focused window is considered last, so a cycle never gets stuck
 * on it while other eligible windows exist; if it is the only eligible window
 * its own index is returned. Returns std::nullopt when no window is eligible.
 *
 * Kept free of wlroots/Wayland types so it can be unit-tested in isolation; the
 * list-walking glue lives in seat.cpp.
 */
std::optional<std::size_t> cycle_next(
		std::size_t count,
		const std::function<bool(std::size_t)> &eligible,
		std::optional<std::size_t> current,
		bool reverse);

/* What the Alt+Tab list orders by: one entry per window, given in
 * most-recently-used order (the focused window first). */
struct SwitcherEntry {
	int stack = 0;          /* 0 = topmost */
	int output_x = 0;       /* its monitor's position in the layout */
	int output_y = 0;
	int x = 0;              /* the window's position */
	int y = 0;
};

/* The order to list `entries` in (indices into it): as given (MRU); top to
 * bottom (Stacking); or monitor by monitor, left to right then top to bottom,
 * and by window position the same way within a monitor (Spatial). Ties keep
 * the MRU order. */
std::vector<std::size_t> switcher_order(const std::vector<SwitcherEntry> &entries,
		SwitcherOrder order);

}  // namespace wb

#endif /* WB_WINDOW_CYCLE_HPP */
