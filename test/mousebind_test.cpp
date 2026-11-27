#include "wb_test.hpp"

#include "waybox/mousebind.hpp"

using namespace wb;

WB_TEST(context_names_parse) {
	WB_CHECK(mouse_context_from_name("Root") == MouseContext::Root);
	WB_CHECK(mouse_context_from_name("Client") == MouseContext::Client);
	WB_CHECK(mouse_context_from_name("Titlebar") == MouseContext::Titlebar);
	WB_CHECK(!mouse_context_from_name("Nonsense").has_value());
	WB_CHECK(!mouse_context_from_name("root").has_value());  /* case sensitive */
}

WB_TEST(event_names_parse) {
	WB_CHECK(mouse_event_from_name("Press") == MouseEvent::Press);
	WB_CHECK(mouse_event_from_name("Click") == MouseEvent::Click);
	WB_CHECK(mouse_event_from_name("DoubleClick") == MouseEvent::DoubleClick);
	WB_CHECK(!mouse_event_from_name("press").has_value());
}

WB_TEST(button_without_modifiers) {
	auto left = parse_mouse_button("Left");
	WB_CHECK(left.has_value());
	WB_CHECK(left->button == MOUSE_BUTTON_LEFT);
	WB_CHECK(left->modifiers == 0);

	WB_CHECK(parse_mouse_button("Right")->button == MOUSE_BUTTON_RIGHT);
	WB_CHECK(parse_mouse_button("Middle")->button == MOUSE_BUTTON_MIDDLE);
}

WB_TEST(button_with_modifiers) {
	auto a_left = parse_mouse_button("A-Left");
	WB_CHECK(a_left.has_value());
	WB_CHECK(a_left->button == MOUSE_BUTTON_LEFT);
	WB_CHECK(a_left->modifiers == (1u << 3));  /* Alt */

	auto cs_right = parse_mouse_button("C-S-Right");
	WB_CHECK(cs_right.has_value());
	WB_CHECK(cs_right->button == MOUSE_BUTTON_RIGHT);
	WB_CHECK(cs_right->modifiers == ((1u << 2) | (1u << 0)));  /* Ctrl|Shift */
}

WB_TEST(button_rejects_unknown) {
	WB_CHECK(!parse_mouse_button("Sideways").has_value());
	WB_CHECK(!parse_mouse_button("A-Bogus").has_value());
	WB_CHECK(!parse_mouse_button("Bogus-Left").has_value());  /* bad modifier */
	WB_CHECK(!parse_mouse_button("").has_value());
}

WB_TEST(binding_matches_exact_tuple) {
	MouseBinding b{MouseContext::Root, MouseEvent::Press, MOUSE_BUTTON_RIGHT, 0, {}};

	WB_CHECK(mouse_binding_matches(b, MouseContext::Root, MOUSE_BUTTON_RIGHT, 0,
			MouseEvent::Press));
	/* wrong context */
	WB_CHECK(!mouse_binding_matches(b, MouseContext::Client, MOUSE_BUTTON_RIGHT, 0,
			MouseEvent::Press));
	/* wrong button */
	WB_CHECK(!mouse_binding_matches(b, MouseContext::Root, MOUSE_BUTTON_LEFT, 0,
			MouseEvent::Press));
	/* wrong event */
	WB_CHECK(!mouse_binding_matches(b, MouseContext::Root, MOUSE_BUTTON_RIGHT, 0,
			MouseEvent::Release));
	/* spurious modifier */
	WB_CHECK(!mouse_binding_matches(b, MouseContext::Root, MOUSE_BUTTON_RIGHT,
			1u << 3, MouseEvent::Press));
}

WB_TEST(binding_matches_with_modifiers) {
	MouseBinding b{MouseContext::Client, MouseEvent::Click, MOUSE_BUTTON_LEFT,
			1u << 3 /* Alt */, {}};
	WB_CHECK(mouse_binding_matches(b, MouseContext::Client, MOUSE_BUTTON_LEFT,
			1u << 3, MouseEvent::Click));
	/* exact modifier mask required */
	WB_CHECK(!mouse_binding_matches(b, MouseContext::Client, MOUSE_BUTTON_LEFT, 0,
			MouseEvent::Click));
}

WB_TEST(scroll_buttons_parse) {
	auto up = parse_mouse_button("Up");
	auto down = parse_mouse_button("A-Down");
	WB_CHECK(up && up->button == wb::MOUSE_BUTTON_UP);
	WB_CHECK(down && down->button == wb::MOUSE_BUTTON_DOWN && down->modifiers != 0);
}

WB_TEST(context_matching) {
	using C = MouseContext;
	WB_CHECK(wb::mouse_context_matches(C::Titlebar, C::Titlebar));
	WB_CHECK(!wb::mouse_context_matches(C::Titlebar, C::Client));
	/* Frame: anywhere on a window. */
	for (C c : {C::Client, C::Titlebar, C::Top, C::Left})
		WB_CHECK(wb::mouse_context_matches(C::Frame, c));
	WB_CHECK(!wb::mouse_context_matches(C::Frame, C::Root));
	/* Desktop covers the empty desktop and desktop surfaces; Root only the
	 * empty desktop (a desktop-icon program keeps its clicks). */
	WB_CHECK(wb::mouse_context_matches(C::Desktop, C::Root));
	WB_CHECK(wb::mouse_context_matches(C::Desktop, C::Desktop));
	WB_CHECK(!wb::mouse_context_matches(C::Root, C::Desktop));
	WB_CHECK(wb::mouse_context_matches(C::Root, C::Root));
	WB_CHECK(!wb::mouse_context_matches(C::Root, C::Client));
	WB_CHECK(!wb::mouse_context_matches(C::Desktop, C::Client));
	WB_CHECK(!wb::mouse_context_matches(C::Frame, C::Desktop));
}

WB_TEST(lock_modifiers_are_ignored) {
	MouseBinding b{MouseContext::Root, MouseEvent::Press, wb::MOUSE_BUTTON_RIGHT, 0, {}};
	const uint32_t caps = 1u << 1, num = 1u << 4, alt = 1u << 3;
	WB_CHECK(mouse_binding_matches(b, MouseContext::Root, wb::MOUSE_BUTTON_RIGHT,
			caps | num, MouseEvent::Press));
	WB_CHECK(!mouse_binding_matches(b, MouseContext::Root, wb::MOUSE_BUTTON_RIGHT,
			alt | num, MouseEvent::Press));
}

WB_TEST(tracker_click_and_double_click) {
	using E = MouseEvent;
	wb::ClickTracker t;
	const uint32_t L = wb::MOUSE_BUTTON_LEFT;
	WB_CHECK(t.press(L, MouseContext::Titlebar, 1, 0, 10, 10, 1000) ==
			std::vector<E>{E::Press});
	WB_CHECK(t.motion(12, 11).empty());  /* under the drag threshold */
	WB_CHECK(t.release(L, 1100) == (std::vector<E>{E::Release, E::Click}));
	t.press(L, MouseContext::Titlebar, 1, 0, 10, 10, 1300);
	WB_CHECK(t.release(L, 1350) ==
			(std::vector<E>{E::Release, E::Click, E::DoubleClick}));
	/* A third quick click starts a new pair. */
	t.press(L, MouseContext::Titlebar, 1, 0, 10, 10, 1400);
	WB_CHECK(t.release(L, 1450) == (std::vector<E>{E::Release, E::Click}));
}

WB_TEST(tracker_double_click_needs_same_place_and_time) {
	using E = MouseEvent;
	const uint32_t L = wb::MOUSE_BUTTON_LEFT;
	wb::ClickTracker slow;
	slow.press(L, MouseContext::Titlebar, 1, 0, 0, 0, 0);
	slow.release(L, 10);
	slow.press(L, MouseContext::Titlebar, 1, 0, 0, 0, 700);
	WB_CHECK(slow.release(L, 710) == (std::vector<E>{E::Release, E::Click}));
	wb::ClickTracker other;
	other.press(L, MouseContext::Titlebar, 1, 0, 0, 0, 0);
	other.release(L, 10);
	other.press(L, MouseContext::Titlebar, 2, 0, 0, 0, 100);  /* another window */
	WB_CHECK(other.release(L, 110) == (std::vector<E>{E::Release, E::Click}));
}

WB_TEST(tracker_drag) {
	using E = MouseEvent;
	wb::ClickTracker t;
	const uint32_t L = wb::MOUSE_BUTTON_LEFT;
	t.press(L, MouseContext::Frame, 1, 0, 0, 0, 0);
	WB_CHECK(t.motion(20, 0) == std::vector<E>{E::Drag});
	WB_CHECK(t.motion(40, 0).empty());  /* once per press */
	WB_CHECK(t.release(L, 50) == std::vector<E>{E::Release});  /* no Click */
	/* Releasing another button, or a forgotten window, gives nothing. */
	t.press(L, MouseContext::Client, 3, 0, 0, 0, 0);
	WB_CHECK(t.release(wb::MOUSE_BUTTON_RIGHT, 10).empty());
	t.forget(3);
	WB_CHECK(!t.pressed() && t.release(L, 20).empty());
}

WB_TEST(wheel_notches_add_up) {
	int32_t acc = 0;
	/* A plain wheel: one notch per 120. */
	WB_CHECK_EQ(wb::wheel_notches(&acc, 120, true, 15.0), 1);
	WB_CHECK_EQ(wb::wheel_notches(&acc, -120, true, -15.0), -1);
	/* A high-resolution wheel: quarter notches add up. */
	acc = 0;
	WB_CHECK_EQ(wb::wheel_notches(&acc, 30, true, 3.75), 0);
	WB_CHECK_EQ(wb::wheel_notches(&acc, 30, true, 3.75), 0);
	WB_CHECK_EQ(wb::wheel_notches(&acc, 30, true, 3.75), 0);
	WB_CHECK_EQ(wb::wheel_notches(&acc, 30, true, 3.75), 1);
	/* Turning back drops the fraction. */
	WB_CHECK_EQ(wb::wheel_notches(&acc, 60, true, 7.5), 0);
	WB_CHECK_EQ(wb::wheel_notches(&acc, -90, true, -11.0), 0);
	WB_CHECK_EQ(wb::wheel_notches(&acc, -30, true, -3.75), -1);
	/* A wheel without step information; touchpads never make notches. */
	acc = 0;
	WB_CHECK_EQ(wb::wheel_notches(&acc, 0, true, -10.0), -1);
	WB_CHECK_EQ(wb::wheel_notches(&acc, 0, false, 10.0), 0);
}
