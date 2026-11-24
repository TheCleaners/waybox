/*
 * The sandbox policy's protocol list. Standard-library only, so it links
 * into the unit test.
 */
#include "waybox/security.hpp"

#include <algorithm>
#include <array>

namespace wb {

namespace {

constexpr std::array<std::string_view, 22> kPrivileged = {
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
	/* Watching whether the user is active. */
	"ext_idle_notifier_v1",
	/* Making further security contexts. */
	"wp_security_context_manager_v1",
};

}  // namespace

bool is_privileged_protocol(std::string_view interface_name) {
	return std::find(kPrivileged.begin(), kPrivileged.end(), interface_name) !=
			kPrivileged.end();
}

}  // namespace wb
