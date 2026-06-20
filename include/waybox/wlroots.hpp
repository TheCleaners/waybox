#ifndef WB_WLROOTS_HPP
#define WB_WLROOTS_HPP

/*
 * Wlroots is a C library and some of its headers (and the libwayland/libdrm
 * headers they pull in) are not directly includable from C++: wlr_scene.h and
 * wlr/render/color.h use C99 `[static N]` array parameters, and
 * wlr_layer_shell_v1.h has a struct field literally named `namespace`.
 *
 * Following Wayfire's approach, we pre-include the C++ standard library first
 * so that the keyword guards below can only ever affect wlroots' own leaf
 * headers and never libstdc++ (whose headers are reached via <math.h>), then
 * include the wlroots headers inside `extern "C"` with the offending keywords
 * neutralised.
 *
 * Include this header instead of the raw wlroots headers anywhere in waybox.
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#define static
#define namespace namespace_
#define class class_

#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/config.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_toplevel_icon_v1.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <wlr/version.h>

#undef class
#undef namespace
#undef static
}

#endif /* WB_WLROOTS_HPP */
