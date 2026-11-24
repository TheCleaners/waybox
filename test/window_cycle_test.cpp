#include "wb_test.hpp"

#include "waybox/window_cycle.hpp"

#include <vector>

using wb::cycle_next;

namespace {

/* Build an eligibility predicate over a fixed vector of flags. */
auto pred(const std::vector<bool> &flags) {
	return [&flags](std::size_t i) { return flags[i]; };
}

}  // namespace

WB_TEST(cycle_empty_returns_nullopt) {
	std::vector<bool> none;
	WB_CHECK(!cycle_next(0, pred(none), std::nullopt, false).has_value());
	WB_CHECK(!cycle_next(0, pred(none), 0, true).has_value());
}

WB_TEST(cycle_forward_advances_and_wraps) {
	std::vector<bool> all{true, true, true};
	WB_CHECK(cycle_next(3, pred(all), 0, false) == std::optional<std::size_t>(1));
	WB_CHECK(cycle_next(3, pred(all), 1, false) == std::optional<std::size_t>(2));
	/* wraps past the end back to 0 */
	WB_CHECK(cycle_next(3, pred(all), 2, false) == std::optional<std::size_t>(0));
}

WB_TEST(cycle_reverse_advances_and_wraps) {
	std::vector<bool> all{true, true, true};
	WB_CHECK(cycle_next(3, pred(all), 2, true) == std::optional<std::size_t>(1));
	WB_CHECK(cycle_next(3, pred(all), 1, true) == std::optional<std::size_t>(0));
	/* wraps past the start back to the end */
	WB_CHECK(cycle_next(3, pred(all), 0, true) == std::optional<std::size_t>(2));
}

WB_TEST(cycle_skips_ineligible) {
	/* only index 0 and 2 are eligible; from 0 forward we must skip 1 -> 2 */
	std::vector<bool> flags{true, false, true};
	WB_CHECK(cycle_next(3, pred(flags), 0, false) == std::optional<std::size_t>(2));
	/* from 2 forward wraps past 1 (ineligible) back to 0 */
	WB_CHECK(cycle_next(3, pred(flags), 2, false) == std::optional<std::size_t>(0));
	/* reverse from 0 skips 2... no, from 0 reverse -> 2 (eligible) */
	WB_CHECK(cycle_next(3, pred(flags), 0, true) == std::optional<std::size_t>(2));
}

WB_TEST(cycle_current_is_last_resort) {
	/* current (index 1) is the only eligible window -> returns itself */
	std::vector<bool> flags{false, true, false};
	WB_CHECK(cycle_next(3, pred(flags), 1, false) == std::optional<std::size_t>(1));
	WB_CHECK(cycle_next(3, pred(flags), 1, true) == std::optional<std::size_t>(1));
}

WB_TEST(cycle_none_eligible_returns_nullopt) {
	std::vector<bool> flags{false, false};
	WB_CHECK(!cycle_next(2, pred(flags), 0, false).has_value());
	WB_CHECK(!cycle_next(2, pred(flags), std::nullopt, false).has_value());
}

WB_TEST(cycle_without_current_starts_from_edge) {
	std::vector<bool> all{true, true, true};
	/* no current, forward -> first eligible from the front */
	WB_CHECK(cycle_next(3, pred(all), std::nullopt, false) == std::optional<std::size_t>(0));
	/* no current, reverse -> first eligible from the back */
	WB_CHECK(cycle_next(3, pred(all), std::nullopt, true) == std::optional<std::size_t>(2));

	std::vector<bool> flags{false, true, false};
	WB_CHECK(cycle_next(3, pred(flags), std::nullopt, false) == std::optional<std::size_t>(1));
	WB_CHECK(cycle_next(3, pred(flags), std::nullopt, true) == std::optional<std::size_t>(1));
}

WB_TEST(switcher_order_mru_stacking_spatial) {
	using wb::SwitcherEntry;
	using wb::SwitcherOrder;
	using wb::switcher_order;
	/* MRU order: focused (a), then b, c, d. Two monitors side by side. */
	std::vector<SwitcherEntry> e = {
		/* stack, output, position */
		{0, 0, 0, 500, 100},    /* a: top, left monitor, right-ish */
		{2, 1920, 0, 1950, 50}, /* b: right monitor */
		{1, 0, 0, 100, 400},    /* c: left monitor, left */
		{3, 0, 0, 100, 100},    /* d: left monitor, left, higher than c */
	};
	using V = std::vector<std::size_t>;
	WB_CHECK(switcher_order(e, SwitcherOrder::MRU) == (V{0, 1, 2, 3}));
	WB_CHECK(switcher_order(e, SwitcherOrder::Stacking) == (V{0, 2, 1, 3}));
	/* Left monitor first: d (100,100), c (100,400), a (500,100); then b. */
	WB_CHECK(switcher_order(e, SwitcherOrder::Spatial) == (V{3, 2, 0, 1}));
	WB_CHECK(switcher_order({}, SwitcherOrder::Spatial).empty());
}
