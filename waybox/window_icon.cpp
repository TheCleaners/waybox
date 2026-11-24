#include "waybox/window_icon.h"

#include <algorithm>
#include <cstring>

#include <drm_fourcc.h>

#include "waybox/frame_view.hpp"
#include "waybox/icon_loader.hpp"
#include "waybox/server.h"
#include "waybox/xdg_shell.h"

/* The icon's size: twice the titlebar button, so it stays sharp on scaled
 * outputs (it is drawn scaled down to fit). */
static int icon_size(struct wb_toplevel *t) {
	int button = t->frame ? t->frame->metrics().button : 16;
	return std::max(button, 8) * 2;
}

void toplevel_update_icon(struct wb_toplevel *t) {
	cairo_surface_t *icon = nullptr;
	if (t->client_icon != nullptr) {
		icon = cairo_surface_reference(t->client_icon);
	} else {
		int size = icon_size(t);
		std::string theme = wb::detect_icon_theme();
		std::string app_id = view_app_id(t);
		std::string lower = app_id;
		std::transform(lower.begin(), lower.end(), lower.begin(),
				[](unsigned char c) { return std::tolower(c); });
		/* Reverse-DNS app IDs (org.example.App) also try their last part,
		 * which is often the desktop file's or the icon's name, but only
		 * after the exact names. Desktop files are only read when needed. */
		std::string last = lower.substr(lower.rfind('.') + 1);
		auto themed = [&](const std::string &name) {
			if (icon == nullptr && !name.empty())
				icon = wb::load_themed_icon(name, theme, size);
		};
		themed(t->client_icon_name);
		if (icon == nullptr)
			themed(wb::desktop_file_icon(app_id));
		themed(app_id);
		themed(lower);
		if (icon == nullptr && last != lower) {
			themed(wb::desktop_file_icon(last));
			themed(last);
		}
	}
	if (t->frame)
		t->frame->set_icon(icon);
	if (t->icon != nullptr)
		cairo_surface_destroy(t->icon);
	t->icon = icon;
}

void toplevel_free_icon(struct wb_toplevel *t) {
	if (t->frame)
		t->frame->set_icon(nullptr);
	if (t->icon != nullptr)
		cairo_surface_destroy(t->icon);
	if (t->client_icon != nullptr)
		cairo_surface_destroy(t->client_icon);
	t->icon = nullptr;
	t->client_icon = nullptr;
}

/* A copy of an ARGB8888/XRGB8888 buffer as a Cairo surface, or nullptr. */
static cairo_surface_t *surface_from_buffer(struct wlr_buffer *buffer) {
	void *data = nullptr;
	uint32_t format = 0;
	size_t stride = 0;
	if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ,
			&data, &format, &stride))
		return nullptr;
	cairo_surface_t *s = nullptr;
	/* An icon is small: refuse anything absurd (and anything Cairo can't
	 * allocate), rather than copying into a surface that isn't there. */
	bool sane = buffer->width > 0 && buffer->height > 0 &&
			buffer->width <= 1024 && buffer->height <= 1024 &&
			stride >= static_cast<size_t>(buffer->width) * 4;
	if (sane && (format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888)) {
		s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, buffer->width, buffer->height);
		if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(s);
			wlr_buffer_end_data_ptr_access(buffer);
			return nullptr;
		}
		unsigned char *dst = cairo_image_surface_get_data(s);
		int dst_stride = cairo_image_surface_get_stride(s);
		for (int y = 0; y < buffer->height; y++) {
			auto *row = reinterpret_cast<uint32_t *>(dst + y * dst_stride);
			std::memcpy(row, static_cast<unsigned char *>(data) + y * stride,
					static_cast<size_t>(buffer->width) * 4);
			if (format == DRM_FORMAT_XRGB8888)
				for (int x = 0; x < buffer->width; x++)
					row[x] |= 0xff000000u;
		}
		cairo_surface_mark_dirty(s);
	}
	wlr_buffer_end_data_ptr_access(buffer);
	return s;
}

static void set_icon(struct wb_server *server, void *data) {
	auto *event = static_cast<struct wlr_xdg_toplevel_icon_manager_v1_set_icon_event *>(data);
	struct wb_toplevel *t = nullptr, *it;
	wl_list_for_each(it, &server->toplevels, link)
		if (it->kind == wb_view_kind::Xdg && it->xdg_toplevel == event->toplevel)
			t = it;
	if (t == nullptr)
		return;
	if (t->client_icon != nullptr)
		cairo_surface_destroy(t->client_icon);
	t->client_icon = nullptr;
	t->client_icon_name.clear();
	if (struct wlr_xdg_toplevel_icon_v1 *icon = event->icon) {
		if (icon->name != nullptr)
			t->client_icon_name = icon->name;
		/* The buffer nearest the size we draw at (scale applied). */
		int want = icon_size(t);
		struct wlr_xdg_toplevel_icon_v1_buffer *best = nullptr, *b;
		int best_d = 0;
		wl_list_for_each(b, &icon->buffers, link) {
			int d = std::abs(b->buffer->width - want);
			if (best == nullptr || d < best_d) {
				best = b;
				best_d = d;
			}
		}
		if (best != nullptr)
			t->client_icon = surface_from_buffer(best->buffer);
	}
	toplevel_update_icon(t);
}

void init_window_icons(struct wb_server *server) {
	server->toplevel_icon_manager =
			wlr_xdg_toplevel_icon_manager_v1_create(server->wl_display, 1);
	int sizes[] = {16, 24, 32, 48, 64};
	wlr_xdg_toplevel_icon_manager_v1_set_sizes(server->toplevel_icon_manager,
			sizes, sizeof(sizes) / sizeof(sizes[0]));
	server->set_toplevel_icon.connect(&server->toplevel_icon_manager->events.set_icon,
			[server](void *data) { set_icon(server, data); });
}
