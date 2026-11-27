/*
 * The sandbox policy's protocol list. Standard-library only, so it links
 * into the unit test.
 */
#include "waybox/security.hpp"

#include <algorithm>
#include <iterator>

namespace wb {

namespace {

constexpr std::string_view kPrivileged[] = {
	/* Seeing or recording other programs and the screen. */
	"ext_image_copy_capture_manager_v1",
	"ext_output_image_capture_source_manager_v1",
	"ext_foreign_toplevel_image_capture_source_manager_v1",
	"zwlr_screencopy_manager_v1",
	"zwlr_export_dmabuf_manager_v1",
	/* Reading and changing the clipboard in the background. */
	"ext_data_control_manager_v1",
	"zwlr_data_control_manager_v1",
	/* Listing and controlling other programs' windows and desktops. */
	"ext_foreign_toplevel_list_v1",
	"zwlr_foreign_toplevel_manager_v1",
	"ext_workspace_manager_v1",
	/* Typing and pointing as the user. */
	"zwp_virtual_keyboard_manager_v1",
	"zwlr_virtual_pointer_manager_v1",
	"zwp_input_method_manager_v2",
	"ext_transient_seat_manager_v1",
	/* Taking over the screen or the session. */
	"zwlr_layer_shell_v1",
	"ext_session_lock_manager_v1",
	"zwlr_output_manager_v1",
	"zwlr_output_power_manager_v1",
	"zwlr_gamma_control_manager_v1",
	"wp_drm_lease_device_v1",
	/* Taking every key binding (lock, Alt+Tab, Exit) while focused. */
	"zwp_keyboard_shortcuts_inhibit_manager_v1",
	/* Watching whether the user is active. */
	"ext_idle_notifier_v1",
	/* Making further security contexts. */
	"wp_security_context_manager_v1",
	/* Xwayland's own (only the Xwayland server gets them at all). */
	"xwayland_shell_v1",
	"zwp_xwayland_keyboard_grab_manager_v1",
};

/* What any program may have: drawing its own windows and taking input
 * for them, and the desktop features that only touch those. A global
 * Waybox makes that's in neither list is refused to sandboxed programs. */
constexpr std::string_view kOrdinary[] = {
	"wl_compositor", "wl_subcompositor", "wl_shm", "wl_seat", "wl_output",
	"wl_data_device_manager", "wl_fixes", "wl_drm", "zwp_linux_dmabuf_v1",
	"wp_linux_drm_syncobj_manager_v1",
	"xdg_wm_base", "zxdg_decoration_manager_v1", "zxdg_output_manager_v1",
	"xdg_activation_v1", "xdg_wm_dialog_v1", "xdg_toplevel_icon_manager_v1",
	"xdg_toplevel_tag_manager_v1", "xdg_toplevel_drag_manager_v1", "xdg_system_bell_v1",
	/* Places only the asking program's own windows. */
	"xdg_session_manager_v1",
	"zxdg_exporter_v1", "zxdg_exporter_v2", "zxdg_importer_v1", "zxdg_importer_v2",
	"wp_viewporter", "wp_presentation", "wp_fractional_scale_manager_v1",
	"wp_single_pixel_buffer_manager_v1", "wp_alpha_modifier_v1",
	"wp_content_type_manager_v1", "wp_cursor_shape_manager_v1",
	"wp_tearing_control_manager_v1", "wp_color_manager_v1",
	/* Within its own surfaces only. */
	"wp_pointer_warp_v1", "zwp_pointer_constraints_v1", "zwp_relative_pointer_manager_v1",
	"zwp_pointer_gestures_v1", "zwp_primary_selection_device_manager_v1",
	"zwp_text_input_manager_v3", "zwp_idle_inhibit_manager_v1",
	"zwp_tablet_manager_v2",
};

}  // namespace

bool is_privileged_protocol(std::string_view interface_name) {
	return std::find(std::begin(kPrivileged), std::end(kPrivileged), interface_name) !=
			std::end(kPrivileged);
}

ProtocolClass classify_protocol(std::string_view interface_name) {
	if (is_privileged_protocol(interface_name))
		return ProtocolClass::Privileged;
	if (std::find(std::begin(kOrdinary), std::end(kOrdinary), interface_name) !=
			std::end(kOrdinary))
		return ProtocolClass::Ordinary;
	return ProtocolClass::Unknown;
}

}  // namespace wb
