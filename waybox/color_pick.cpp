#include "color_pick.h"

#include <algorithm>

#include <drm_fourcc.h>
#include <xkbcommon/xkbcommon.h>

#include "waybox/wlroots.hpp"
#include "waybox/cursor.h"
#include "waybox/mousebind.hpp"
#include "waybox/server.h"
#include "waybox/session_lock.h"

namespace {

constexpr int kTimeoutMs = 30000;

struct Pick {
	struct wb_server *server = nullptr;
	ColorPicked done;
	struct wl_event_source *timer = nullptr;
};

Pick *pick = nullptr;
/* The button whose press ended a pick: its release is the pick's too,
 * not a stray release for whatever is under the pointer. */
uint32_t ended_by = 0;

void end(bool ok, const wb::Color &c, const std::string &error) {
	if (pick == nullptr)
		return;
	Pick *p = pick;
	pick = nullptr;  /* first: `done` may start another */
	if (p->timer != nullptr)
		wl_event_source_remove(p->timer);
	struct wb_server *server = p->server;
	ColorPicked done = std::move(p->done);
	delete p;
	server->cursor->show_xcursor("default");
	wlr_log(WLR_INFO, "wb-pick %s", ok ? "picked" : error.c_str());
	if (done)
		done(ok, c, error);
}

/* The pixel at layout x, y, as shown: that output's frame rendered now,
 * without the cursor, read, and dropped. */
bool read_pixel(struct wb_server *server, double lx, double ly, wb::Color *out,
		std::string *error) {
	struct wlr_output *wo = wlr_output_layout_output_at(server->output_layout, lx, ly);
	if (wo == nullptr) {
		*error = "no monitor there";
		return false;
	}
	struct wlr_scene_output *so = wlr_scene_get_scene_output(server->scene, wo);
	if (so == nullptr) {
		*error = "no monitor there";
		return false;
	}
	struct wlr_box box;
	wlr_output_layout_get_box(server->output_layout, wo, &box);
	/* Into the output's pixels, then (turned and flipped back) the
	 * buffer's, as screencopy does. */
	int tw, th;
	wlr_output_transformed_resolution(wo, &tw, &th);
	struct wlr_box px = {static_cast<int>((lx - box.x) * wo->scale),
			static_cast<int>((ly - box.y) * wo->scale), 1, 1};
	px.x = std::clamp(px.x, 0, std::max(0, tw - 1));
	px.y = std::clamp(px.y, 0, std::max(0, th - 1));
	struct wlr_box in_buffer;
	wlr_box_transform(&in_buffer, &px, wlr_output_transform_invert(wo->transform), tw, th);

	/* The cursor isn't part of what's under it. */
	wlr_cursor_unset_image(server->cursor->cursor);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	bool ok = wlr_scene_output_build_state(so, &state, nullptr) && state.buffer != nullptr;
	if (ok) {
		struct wlr_texture *tex = wlr_texture_from_buffer(server->renderer, state.buffer);
		uint32_t pixel = 0;
		ok = tex != nullptr;
		if (ok) {
			const struct wlr_texture_read_pixels_options opts = {
				.data = &pixel, .format = DRM_FORMAT_ARGB8888, .stride = 4,
				.dst_x = 0, .dst_y = 0, .src_box = in_buffer,
			};
			ok = wlr_texture_read_pixels(tex, &opts);
			wlr_texture_destroy(tex);
		}
		if (ok) {
			out->a = 255;
			out->r = static_cast<uint8_t>(pixel >> 16);
			out->g = static_cast<uint8_t>(pixel >> 8);
			out->b = static_cast<uint8_t>(pixel);
		}
	}
	wlr_output_state_finish(&state);  /* never shown */
	if (!ok)
		*error = "could not read the screen there";
	return ok;
}

}  // namespace

bool color_pick_active(struct wb_server *) {
	return pick != nullptr;
}

bool color_pick_start(struct wb_server *server, ColorPicked done) {
	if (pick != nullptr || session_locked(server))
		return false;
	ended_by = 0;
	pick = new Pick{server, std::move(done)};
	pick->timer = wl_event_loop_add_timer(server->wl_event_loop, [](void *) {
		end(false, {}, "timed out");
		return 0;
	}, nullptr);
	if (pick->timer != nullptr)
		wl_event_source_timer_update(pick->timer, kTimeoutMs);
	/* Nothing under the pointer is pointed at while picking. */
	wlr_seat_pointer_clear_focus(server->seat->seat);
	server->cursor->show_xcursor("crosshair");
	wlr_log(WLR_INFO, "%s", "wb-pick start");
	return true;
}

void color_pick_cancel(struct wb_server *, const std::string &why) {
	end(false, {}, why);
}

bool color_pick_motion(struct wb_server *server) {
	if (pick == nullptr)
		return false;
	wlr_seat_pointer_clear_focus(server->seat->seat);
	server->cursor->show_xcursor("crosshair");
	return true;
}

bool color_pick_button(struct wb_server *server, uint32_t button, bool pressed) {
	if (pick == nullptr) {
		if (!pressed && ended_by != 0 && button == ended_by) {
			ended_by = 0;
			return true;
		}
		return false;
	}
	if (!pressed)
		return true;  /* (the press was the pick's: so is its release) */
	ended_by = button;
	if (button == wb::MOUSE_BUTTON_LEFT) {
		wb::Color c;
		std::string error;
		if (read_pixel(server, server->cursor->cursor->x, server->cursor->cursor->y, &c, &error))
			end(true, c, "");
		else
			end(false, {}, error);
	} else {
		end(false, {}, "cancelled");
	}
	return true;
}

bool color_pick_key(struct wb_server *, uint32_t keysym) {
	if (pick == nullptr)
		return false;
	if (keysym == XKB_KEY_Escape) {
		end(false, {}, "cancelled");
		return true;
	}
	return false;
}

void finish_color_pick(struct wb_server *) {
	if (pick == nullptr)
		return;
	Pick *p = pick;
	pick = nullptr;
	if (p->timer != nullptr)
		wl_event_source_remove(p->timer);
	delete p;  /* (no reply: Waybox is going) */
}
