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
