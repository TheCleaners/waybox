#include "wb_test.hpp"

#include "waybox/widget.hpp"

using wb::Color;
using wb::ControlStyle;
using wb::StateStyle;
using wb::WidgetState;

WB_TEST(insets_sum_helpers) {
	wb::Insets in{2, 4, 6, 8};  /* top, right, bottom, left */
	WB_CHECK(in.horizontal() == 12);
	WB_CHECK(in.vertical() == 8);
}

WB_TEST(control_for_state_selects_and_falls_back) {
	ControlStyle c;
	c.normal.fg = Color{1, 1, 1, 255};
	c.hover.fg = Color{2, 2, 2, 255};
	c.pressed.fg = Color{3, 3, 3, 255};
	c.disabled.fg = Color{4, 4, 4, 255};

	WB_CHECK(c.for_state(WidgetState::Normal).fg == (Color{1, 1, 1, 255}));
	WB_CHECK(c.for_state(WidgetState::Hover).fg == (Color{2, 2, 2, 255}));
	WB_CHECK(c.for_state(WidgetState::Pressed).fg == (Color{3, 3, 3, 255}));
	WB_CHECK(c.for_state(WidgetState::Disabled).fg == (Color{4, 4, 4, 255}));
}
