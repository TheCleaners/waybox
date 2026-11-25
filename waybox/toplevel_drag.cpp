/*
 * xdg-toplevel-drag-v1 (wlroots has none). A client ties a toplevel to the
 * wl_data_source of a drag; while that drag goes on, the toplevel follows
 * the pointer at the given offset (from its window geometry), and it takes
 * no part in finding the drop target under the pointer. When the drag
 * ends, it stays where it is.
 */
#include "toplevel_drag.h"

#include <list>
#include <memory>

#include "waybox/listener.hpp"
#include "waybox/seat.h"
#include "waybox/cursor.h"
#include "waybox/xdg_shell.h"

#include "xdg-toplevel-drag-v1-protocol.h"

namespace {

struct ToplevelDrag {
	struct wb_server *server = nullptr;
	struct wl_resource *resource = nullptr;
	struct wlr_data_source *source = nullptr;
	struct wlr_xdg_toplevel *toplevel = nullptr;  /* attached, if any */
	int x_offset = 0, y_offset = 0;
	wb::Listener source_destroy;
	wb::Listener toplevel_destroy;
};

std::list<std::unique_ptr<ToplevelDrag>> drags;

ToplevelDrag *from_resource(struct wl_resource *resource) {
	return static_cast<ToplevelDrag *>(wl_resource_get_user_data(resource));
}

void detach(ToplevelDrag *d) {
	d->toplevel = nullptr;
	d->toplevel_destroy.disconnect();
}

/* The drag of the drag-and-drop in progress, if it's one of ours. */
ToplevelDrag *active(struct wb_server *server) {
	struct wlr_drag *drag = server->seat->seat->drag;
	if (drag == nullptr || drag->source == nullptr)
		return nullptr;
	for (auto &d : drags)
		if (d->source == drag->source)
			return d.get();
	return nullptr;
}

struct wb_toplevel *window_of(struct wb_server *server, struct wlr_xdg_toplevel *xdg) {
	if (xdg == nullptr)
		return nullptr;
	struct wb_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link)
		if (t->xdg_toplevel == xdg)
			return t;
	return nullptr;
}

void handle_destroy(struct wl_client *, struct wl_resource *resource) {
	ToplevelDrag *d = from_resource(resource);
	if (d != nullptr && d->source != nullptr && active(d->server) == d) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_V1_ERROR_ONGOING_DRAG,
				"destroyed during the drag");
		return;
	}
	wl_resource_destroy(resource);
}

void handle_attach(struct wl_client *, struct wl_resource *resource,
		struct wl_resource *toplevel_resource, int32_t x_offset, int32_t y_offset) {
	ToplevelDrag *d = from_resource(resource);
	if (d == nullptr)
		return;
	struct wlr_xdg_toplevel *xdg = wlr_xdg_toplevel_from_resource(toplevel_resource);
	if (d->toplevel != nullptr && d->toplevel != xdg) {
		wl_resource_post_error(resource, XDG_TOPLEVEL_DRAG_V1_ERROR_TOPLEVEL_ATTACHED,
				"a toplevel is already attached");
		return;
	}
	if (xdg == nullptr)
		return;  /* its role is gone: nothing to carry */
	d->toplevel = xdg;
	d->x_offset = x_offset;
	d->y_offset = y_offset;
	d->toplevel_destroy.connect(&xdg->events.destroy, [d](void *) { detach(d); });
	/* An existing window starts following now. */
	toplevel_drag_motion(d->server);
}

const struct xdg_toplevel_drag_v1_interface drag_impl = {
	.destroy = handle_destroy,
	.attach = handle_attach,
};

void drag_resource_destroy(struct wl_resource *resource) {
	ToplevelDrag *d = from_resource(resource);
	if (d == nullptr)
		return;
	drags.remove_if([d](const auto &p) { return p.get() == d; });
}

void manager_destroy(struct wl_client *, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

void manager_get_drag(struct wl_client *client, struct wl_resource *manager,
		uint32_t id, struct wl_resource *source_resource) {
	auto *server = static_cast<struct wb_server *>(wl_resource_get_user_data(manager));
	/* wlroots' client data source starts with its wlr_data_source; null once
	 * the source is gone (the drag object is then inert). */
	auto *source = static_cast<struct wlr_data_source *>(
			wl_resource_get_user_data(source_resource));
	for (auto &d : drags) {
		if (source != nullptr && d->source == source) {
			wl_resource_post_error(manager, XDG_TOPLEVEL_DRAG_MANAGER_V1_ERROR_INVALID_SOURCE,
					"the data source already has a toplevel drag");
			return;
		}
	}
	struct wl_resource *resource = wl_resource_create(client,
			&xdg_toplevel_drag_v1_interface, wl_resource_get_version(manager), id);
	if (resource == nullptr) {
		wl_client_post_no_memory(client);
		return;
	}
	auto d = std::make_unique<ToplevelDrag>();
	d->server = server;
	d->resource = resource;
	d->source = source;
	ToplevelDrag *raw = d.get();
	if (source != nullptr)
		raw->source_destroy.connect(&source->events.destroy, [raw](void *) {
			raw->source = nullptr;
			raw->source_destroy.disconnect();
			detach(raw);
		});
	drags.push_back(std::move(d));
	wl_resource_set_implementation(resource, &drag_impl, raw, drag_resource_destroy);
}

const struct xdg_toplevel_drag_manager_v1_interface manager_impl = {
	.destroy = manager_destroy,
	.get_xdg_toplevel_drag = manager_get_drag,
};

void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
			&xdg_toplevel_drag_manager_v1_interface, static_cast<int>(version), id);
	if (resource == nullptr) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

}  // namespace

void init_toplevel_drag(struct wb_server *server) {
	if (wl_global_create(server->wl_display, &xdg_toplevel_drag_manager_v1_interface,
			1, server, bind_manager) == nullptr)
		wlr_log(WLR_ERROR, "%s", "Couldn't create the xdg-toplevel-drag global");
}

struct wb_toplevel *toplevel_drag_window(struct wb_server *server) {
	ToplevelDrag *d = active(server);
	if (d == nullptr)
		return nullptr;
	struct wb_toplevel *t = window_of(server, d->toplevel);
	return t != nullptr && t->mapped ? t : nullptr;
}

void toplevel_drag_motion(struct wb_server *server) {
	ToplevelDrag *d = active(server);
	struct wb_toplevel *t = toplevel_drag_window(server);
	if (d == nullptr || t == nullptr)
		return;
	t->geometry.x = static_cast<int>(server->cursor->cursor->x) - d->x_offset;
	t->geometry.y = static_cast<int>(server->cursor->cursor->y) - d->y_offset;
	position_toplevel(t);
}

bool toplevel_drag_place(struct wb_toplevel *toplevel) {
	struct wb_server *server = toplevel->server;
	ToplevelDrag *d = active(server);
	if (d == nullptr || toplevel->xdg_toplevel == nullptr ||
			d->toplevel != toplevel->xdg_toplevel)
		return false;
	toplevel->geometry.x = static_cast<int>(server->cursor->cursor->x) - d->x_offset;
	toplevel->geometry.y = static_cast<int>(server->cursor->cursor->y) - d->y_offset;
	position_toplevel(toplevel);
	return true;
}

void toplevel_drag_unmapped(struct wb_toplevel *toplevel) {
	if (toplevel->xdg_toplevel == nullptr)
		return;
	for (auto &d : drags)
		if (d->toplevel == toplevel->xdg_toplevel)
			detach(d.get());
}
