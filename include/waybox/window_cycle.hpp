#ifndef WB_WINDOW_CYCLE_HPP
#define WB_WINDOW_CYCLE_HPP

#include <cstddef>
#include <functional>
#include <optional>

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

}  // namespace wb

#endif /* WB_WINDOW_CYCLE_HPP */
