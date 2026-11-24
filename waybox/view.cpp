/*
 * Per-kind window operations: xdg-shell toplevels and (when wlroots has
 * it) Xwayland windows. Everything outside the shell files goes through
 * these instead of the xdg/X11 objects.
 */
#include "waybox/wlroots.hpp"

#include "waybox/xdg_shell.h"

static bool is_x11(struct wb_toplevel *t) {
#if WLR_HAS_XWAYLAND
	return t->kind == wb_view_kind::Xwayland && t->xsurface != nullptr;
#else
	(void) t;
	return false;
#endif
}

const char *view_title(struct wb_toplevel *t) {
	const char *s = nullptr;
#if WLR_HAS_XWAYLAND
	if (is_x11(t))
		s = t->xsurface->title;
	else
#endif
	if (t->xdg_toplevel != nullptr)
		s = t->xdg_toplevel->title;
	return s != nullptr ? s : "";
}

const char *view_app_id(struct wb_toplevel *t) {
	const char *s = nullptr;
#if WLR_HAS_XWAYLAND
	if (is_x11(t))
		s = t->xsurface->class_;
	else
#endif
	if (t->xdg_toplevel != nullptr)
		s = t->xdg_toplevel->app_id;
	return s != nullptr ? s : "";
}

struct wlr_surface *view_surface(struct wb_toplevel *t) {
#if WLR_HAS_XWAYLAND
	if (is_x11(t))
		return t->xsurface->surface;
#endif
	return t->xdg_toplevel != nullptr ? t->xdg_toplevel->base->surface : nullptr;
}

bool view_ready(struct wb_toplevel *t) {
#if WLR_HAS_XWAYLAND
	if (is_x11(t))
		return t->xsurface->surface != nullptr;
#endif
	return t->xdg_toplevel != nullptr && t->xdg_toplevel->base->initialized;
}

struct wlr_box view_geometry(struct wb_toplevel *t) {
#if WLR_HAS_XWAYLAND
	if (is_x11(t))
		return {0, 0, t->xsurface->width, t->xsurface->height};
#endif
	if (t->xdg_toplevel == nullptr)
		return {};
	return t->xdg_toplevel->base->geometry;
}

void view_size_hints(struct wb_toplevel *t, int *min_w, int *min_h,
		int *max_w, int *max_h) {
	*min_w = *min_h = *max_w = *max_h = 0;
#if WLR_HAS_XWAYLAND
	if (is_x11(t)) {
		xcb_size_hints_t *h = t->xsurface->size_hints;
		if (h == nullptr)
			return;
		if (h->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) {
			*min_w = h->min_width > 0 ? h->min_width : 0;
			*min_h = h->min_height > 0 ? h->min_height : 0;
		}
		if (h->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) {
			*max_w = h->max_width > 0 ? h->max_width : 0;
			*max_h = h->max_height > 0 ? h->max_height : 0;
		}
		return;
	}
#endif
	if (t->xdg_toplevel == nullptr)
		return;
	*min_w = t->xdg_toplevel->current.min_width;
	*min_h = t->xdg_toplevel->current.min_height;
	*max_w = t->xdg_toplevel->current.max_width;
	*max_h = t->xdg_toplevel->current.max_height;
}

void view_set_size(struct wb_toplevel *t, int width, int height) {
	if (!view_ready(t))
		return;
#if WLR_HAS_XWAYLAND
	if (is_x11(t)) {
		wlr_xwayland_surface_configure(t->xsurface,
				static_cast<int16_t>(t->geometry.x),
				static_cast<int16_t>(t->geometry.y),
				static_cast<uint16_t>(width > 0 ? width : 1),
				static_cast<uint16_t>(height > 0 ? height : 1));
		return;
	}
#endif
	wlr_xdg_toplevel_set_size(t->xdg_toplevel, width, height);
}

void view_moved(struct wb_toplevel *t) {
#if WLR_HAS_XWAYLAND
	if (!is_x11(t) || !view_ready(t))
		return;
	struct wlr_xwayland_surface *xs = t->xsurface;
	if (xs->x == t->geometry.x && xs->y == t->geometry.y)
		return;
	wlr_xwayland_surface_configure(xs, static_cast<int16_t>(t->geometry.x),
			static_cast<int16_t>(t->geometry.y), xs->width, xs->height);
#else
	(void) t;
#endif
}

void view_set_activated(struct wb_toplevel *t, bool activated) {
	if (!view_ready(t))
		return;
#if WLR_HAS_XWAYLAND
	if (is_x11(t)) {
		wlr_xwayland_surface_activate(t->xsurface, activated);
		if (activated)
			wlr_xwayland_surface_restack(t->xsurface, nullptr,
					XCB_STACK_MODE_ABOVE);
		return;
	}
#endif
	wlr_xdg_toplevel_set_activated(t->xdg_toplevel, activated);
}

void view_set_maximized(struct wb_toplevel *t, bool horz, bool vert) {
	if (!view_ready(t))
		return;
#if WLR_HAS_XWAYLAND
	if (is_x11(t)) {
		wlr_xwayland_surface_set_maximized(t->xsurface, horz, vert);
		return;
	}
#endif
	/* xdg-shell only has a single "maximized" state. */
	wlr_xdg_toplevel_set_maximized(t->xdg_toplevel, horz && vert);
}

void view_set_fullscreen(struct wb_toplevel *t, bool fullscreen) {
	if (!view_ready(t))
		return;
#if WLR_HAS_XWAYLAND
	if (is_x11(t)) {
		wlr_xwayland_surface_set_fullscreen(t->xsurface, fullscreen);
		return;
	}
#endif
	wlr_xdg_toplevel_set_fullscreen(t->xdg_toplevel, fullscreen);
}

void view_set_minimized(struct wb_toplevel *t, bool minimized) {
#if WLR_HAS_XWAYLAND
	if (is_x11(t) && view_ready(t))
		wlr_xwayland_surface_set_minimized(t->xsurface, minimized);
#else
	(void) t;
	(void) minimized;
#endif
	/* xdg-shell has no minimized state to report. */
}

void view_close(struct wb_toplevel *t) {
#if WLR_HAS_XWAYLAND
	if (is_x11(t)) {
		wlr_xwayland_surface_close(t->xsurface);
		return;
	}
#endif
	if (t->xdg_toplevel != nullptr)
		wlr_xdg_toplevel_send_close(t->xdg_toplevel);
}

struct wb_toplevel *view_parent(struct wb_toplevel *t) {
	struct wlr_surface *parent_surface = nullptr;
#if WLR_HAS_XWAYLAND
	if (is_x11(t)) {
		if (t->xsurface->parent != nullptr)
			parent_surface = t->xsurface->parent->surface;
	} else
#endif
	if (t->xdg_toplevel != nullptr && t->xdg_toplevel->parent != nullptr)
		parent_surface = t->xdg_toplevel->parent->base->surface;
	if (parent_surface == nullptr)
		return nullptr;
	return toplevel_from_surface(t->server, parent_surface);
}

bool view_is_modal(struct wb_toplevel *t) {
#if WLR_HAS_XWAYLAND
	if (is_x11(t))
		return t->xsurface->modal;
#endif
	if (t->xdg_toplevel == nullptr)
		return false;
	struct wlr_xdg_dialog_v1 *dialog =
			wlr_xdg_dialog_v1_try_from_wlr_xdg_toplevel(t->xdg_toplevel);
	return dialog != nullptr && dialog->modal;
}

struct wb_toplevel *view_modal_child(struct wb_toplevel *t) {
	struct wb_toplevel *found = nullptr;
	for (int depth = 0; depth < 8; depth++) {  /* parent cycles can't loop */
		struct wb_toplevel *next = nullptr, *c;
		wl_list_for_each(c, &t->server->toplevels, link)
			if (c != t && c->mapped && !c->minimized && view_parent(c) == t &&
					view_is_modal(c) &&
					(c->omnipresent || t->omnipresent || c->desktop == t->desktop)) {
				next = c;
				break;
			}
		if (next == nullptr)
			break;
		found = t = next;
	}
	return found;
}

struct wb_toplevel *toplevel_from_surface(struct wb_server *server,
		struct wlr_surface *surface) {
	if (surface == nullptr)
		return nullptr;
	struct wb_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (view_surface(t) == surface)
			return t;
	}
	return nullptr;
}
