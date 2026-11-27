#include "about.h"

#include "waybox/wlroots.hpp"
extern "C" {
#include <wlr/render/gles2.h>
#include <wlr/render/pixman.h>
#include <wlr/version.h>
#if __has_include(<wlr/render/vulkan.h>)
#include <wlr/render/vulkan.h>
#define WB_HAVE_VULKAN_HEADER 1
#endif
}

#include "waybox/server.h"
#include "version.h"

wb::AboutInfo about_info(struct wb_server *server) {
	wb::AboutInfo info;
	info.version = WB_BUILD_VERSION;
	info.wlroots = WLR_VERSION_STR;
	struct wlr_renderer *r = server->renderer;
	if (r == nullptr)
		info.renderer = "none";
	else if (wlr_renderer_is_gles2(r))
		info.renderer = "GPU (OpenGL ES)";
#ifdef WB_HAVE_VULKAN_HEADER
	else if (wlr_renderer_is_vk(r))
		info.renderer = "GPU (Vulkan)";
#endif
	else if (wlr_renderer_is_pixman(r))
		info.renderer = "software";
	else
		info.renderer = "other";
	info.xwayland = server->xwayland != nullptr;
	info.monitors = wl_list_length(&server->outputs);
	return info;
}
