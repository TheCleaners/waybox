/*
 * Pure window-cycle selection. Depends only on the C++ standard library so it
 * can be linked into the standalone unit test without wlroots.
 */
#include "waybox/window_cycle.hpp"

namespace wb {

std::optional<std::size_t> cycle_next(
		std::size_t count,
		const std::function<bool(std::size_t)> &eligible,
		std::optional<std::size_t> current,
		bool reverse) {
	if (count == 0)
		return std::nullopt;

	/* Scan all `count` candidates in order, starting just past `current` in the
	 * chosen direction and wrapping. With a current index the last candidate
	 * examined is `current` itself, so it is only selected when nothing else is
	 * eligible. */
	for (std::size_t step = 1; step <= count; ++step) {
		std::size_t idx;
		if (current.has_value()) {
			const std::size_t c = *current % count;
			idx = reverse ? (c + count - (step % count)) % count
			              : (c + step) % count;
		} else {
			idx = reverse ? (count - step) : (step - 1);
		}
		if (eligible(idx))
			return idx;
	}
	return std::nullopt;
}

}  // namespace wb
