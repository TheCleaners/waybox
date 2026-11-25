/*
 * wl_fixes (core protocol, libwayland 1.24+): requests that fix core
 * interfaces which can't be changed in place.
 *
 * - destroy_registry: wl_registry has no destructor of its own, so a
 *   client's registries were never freed on the server side.
 * - ack_global_remove (version 2): a client acknowledges a global's removal
 *   (an output unplugged); libwayland then knows no bind for it can still
 *   be on its way, instead of a client being disconnected for binding a
 *   global that just went away.
 *
 * Waybox's own, rather than wlroots 0.20's wlr_fixes (version 1 only, and
 * not in 0.19).
 */
#include "waybox/wlroots.hpp"

#include "fixes.h"

namespace {

constexpr uint32_t kVersion = 2;

void handle_destroy(struct wl_client *, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

void handle_destroy_registry(struct wl_client *, struct wl_resource *,
		struct wl_resource *registry) {
	wl_resource_destroy(registry);
}

void handle_ack_global_remove(struct wl_client *, struct wl_resource *resource,
		struct wl_resource *registry, uint32_t name) {
	wl_fixes_handle_ack_global_remove(resource, registry, name);
}

const struct wl_fixes_interface fixes_impl = {
	.destroy = handle_destroy,
	.destroy_registry = handle_destroy_registry,
	.ack_global_remove = handle_ack_global_remove,
};

void bind_fixes(struct wl_client *client, void *, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client, &wl_fixes_interface,
			static_cast<int>(version), id);
	if (resource == nullptr) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &fixes_impl, nullptr, nullptr);
}

}  // namespace

void init_fixes(struct wb_server *server) {
	uint32_t version = std::min<uint32_t>(kVersion,
			static_cast<uint32_t>(wl_fixes_interface.version));
	if (wl_global_create(server->wl_display, &wl_fixes_interface,
			static_cast<int>(version), nullptr, bind_fixes) == nullptr)
		wlr_log(WLR_ERROR, "%s", "Couldn't create the wl_fixes global");
}
