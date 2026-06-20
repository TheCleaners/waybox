#include "wb_test.hpp"

#include "waybox/decor_mode.hpp"

using wb::DecorMode;
using wb::NegotiatedDecoration;
using wb::negotiate_decoration;
using wb::toggle_decor;

WB_TEST(none_always_resolves_client_side) {
	WB_CHECK(negotiate_decoration(DecorMode::None, true, true) ==
			NegotiatedDecoration::ClientSide);
	WB_CHECK(negotiate_decoration(DecorMode::None, false, false) ==
			NegotiatedDecoration::ClientSide);
}

WB_TEST(full_honours_ssd_only_when_available) {
	WB_CHECK(negotiate_decoration(DecorMode::Full, false, true) ==
			NegotiatedDecoration::ServerSide);
	/* no SSD renderer => never claim server-side (would leave no frame) */
	WB_CHECK(negotiate_decoration(DecorMode::Full, true, false) ==
			NegotiatedDecoration::ClientSide);
}

WB_TEST(default_follows_client_when_ssd_available) {
	WB_CHECK(negotiate_decoration(DecorMode::Default, true, true) ==
			NegotiatedDecoration::ServerSide);
	WB_CHECK(negotiate_decoration(DecorMode::Default, false, true) ==
			NegotiatedDecoration::ClientSide);
	/* without SSD support, always client-side regardless of client request */
	WB_CHECK(negotiate_decoration(DecorMode::Default, true, false) ==
			NegotiatedDecoration::ClientSide);
}

WB_TEST(toggle_flips_decorated_and_undecorated) {
	WB_CHECK(toggle_decor(DecorMode::Full) == DecorMode::None);
	/* Default counts as decorated, so the first toggle hides decorations */
	WB_CHECK(toggle_decor(DecorMode::Default) == DecorMode::None);
	WB_CHECK(toggle_decor(DecorMode::None) == DecorMode::Full);
	/* toggling twice from a decorated state returns to decorated */
	WB_CHECK(toggle_decor(toggle_decor(DecorMode::Full)) == DecorMode::Full);
}
