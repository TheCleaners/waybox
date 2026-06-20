#include "decoration.h"
#include "waybox/xdg_shell.h"

static void handle_xdg_decoration_destroy(struct wl_listener *listener, void *data) {
	struct wb_decoration *decoration = wl_container_of(listener, decoration, destroy);
	wl_list_remove(&decoration->destroy.link);
	wl_list_remove(&decoration->request_mode.link);
	free(decoration);
}

static void handle_xdg_decoration_mode(struct wl_listener *listener, void *data) {
	struct wb_decoration *decoration = wl_container_of(listener, decoration, request_mode);
	struct wlr_xdg_toplevel_decoration_v1 *toplevel_decoration =
		decoration->toplevel_decoration;
	struct wlr_xdg_toplevel *xdg_toplevel = toplevel_decoration->toplevel;

	/* Resolve the wb_toplevel that actually owns this decoration, rather than
	 * assuming it is the front toplevel. */
	struct wlr_scene_tree *scene_tree = xdg_toplevel->base->data;
	struct wb_toplevel *toplevel = scene_tree ? scene_tree->node.data : NULL;

	if (xdg_toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(toplevel_decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
	if (toplevel != NULL)
		toplevel->decoration = toplevel_decoration;
}

static void handle_new_xdg_toplevel_decoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *toplevel_decoration = data;
	struct wb_decoration *decoration = calloc(1, sizeof(struct wb_decoration));
	if (decoration == NULL)
		return;
	struct wb_server *server = wl_container_of(listener, server, new_xdg_decoration);
	decoration->server = server;
	decoration->toplevel_decoration = toplevel_decoration;

	decoration->request_mode.notify = handle_xdg_decoration_mode;
	wl_signal_add(&toplevel_decoration->events.request_mode, &decoration->request_mode);
	decoration->destroy.notify = handle_xdg_decoration_destroy;
	wl_signal_add(&toplevel_decoration->events.destroy, &decoration->destroy);
}

void init_xdg_decoration(struct wb_server *server) {
	struct wlr_xdg_decoration_manager_v1 *decoration = wlr_xdg_decoration_manager_v1_create(server->wl_display);
	server->new_xdg_decoration.notify = handle_new_xdg_toplevel_decoration;
	wl_signal_add(&decoration->events.new_toplevel_decoration, &server->new_xdg_decoration);
}
