#include "wb_test.hpp"

#include "waybox/security.hpp"

using wb::is_privileged_protocol;

WB_TEST(capture_input_and_session_control_are_privileged) {
	for (const char *name : {"ext_image_copy_capture_manager_v1",
			"ext_foreign_toplevel_image_capture_source_manager_v1",
			"zwlr_screencopy_manager_v1", "ext_data_control_manager_v1",
			"zwp_virtual_keyboard_manager_v1", "zwlr_virtual_pointer_manager_v1",
			"zwlr_layer_shell_v1", "ext_session_lock_manager_v1",
			"zwlr_output_manager_v1", "zwlr_foreign_toplevel_manager_v1",
			"wp_security_context_manager_v1"})
		WB_CHECK(is_privileged_protocol(name));
}

WB_TEST(ordinary_app_protocols_are_not) {
	for (const char *name : {"wl_compositor", "wl_seat", "wl_shm", "xdg_wm_base",
			"zwp_linux_dmabuf_v1", "wp_fractional_scale_manager_v1",
			"xdg_activation_v1", "zxdg_decoration_manager_v1",
			"wp_color_manager_v1", "zwp_pointer_constraints_v1",
			"zwp_idle_inhibit_manager_v1", "wp_cursor_shape_manager_v1"})
		WB_CHECK(!is_privileged_protocol(name));
	WB_CHECK(!is_privileged_protocol(""));
}

/* Every global Waybox makes is one or the other; a new one is unknown
 * (and so refused to sandboxed programs) until classified. */
WB_TEST(protocols_classified) {
	using wb::ProtocolClass;
	using wb::classify_protocol;
	WB_CHECK(classify_protocol("wl_compositor") == ProtocolClass::Ordinary);
	WB_CHECK(classify_protocol("xdg_session_manager_v1") == ProtocolClass::Ordinary);
	WB_CHECK(classify_protocol("zwlr_screencopy_manager_v1") == ProtocolClass::Privileged);
	WB_CHECK(classify_protocol("xwayland_shell_v1") == ProtocolClass::Privileged);
	WB_CHECK(classify_protocol("zwp_fancy_new_thing_v1") == ProtocolClass::Unknown);
	WB_CHECK(classify_protocol("") == ProtocolClass::Unknown);
}
