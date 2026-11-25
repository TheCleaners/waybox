#include "bell.h"

#include <list>
#include <memory>

#include "waybox/wlroots.hpp"
#include "waybox/listener.hpp"
#include "waybox/cursor.h"
#include "waybox/xdg_shell.h"
#include "config.h"
#include "waybox/session_lock.h"

namespace {

constexpr int kFlashMs = 150;
constexpr uint32_t kCommandGapMs = 200;  /* a burst of rings runs it once */

/* A flash on screen, removed by its timer (or gone with its window). */
struct Flash {
	struct wlr_scene_rect *rect = nullptr;
	struct wl_event_source *timer = nullptr;
	wb::Listener rect_destroy;
};

struct BellState {
	struct wlr_xdg_system_bell_v1 *bell = nullptr;
	wb::Listener ring, destroy;
	std::list<std::unique_ptr<Flash>> flashes;
	uint32_t last_command = 0;
	bool commanded = false;
};

BellState *state = nullptr;

uint32_t now_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void drop(Flash *f) {
	if (f->timer != nullptr)
		wl_event_source_remove(f->timer);
	f->rect_destroy.disconnect();
	if (f->rect != nullptr)
		wlr_scene_node_destroy(&f->rect->node);
	state->flashes.remove_if([f](const auto &p) { return p.get() == f; });
}

void flash(struct wb_server *server, struct wlr_scene_tree *parent, int x, int y,
		int w, int h) {
	if (w <= 0 || h <= 0)
		return;
	static const float colour[4] = {0.5f, 0.5f, 0.5f, 0.5f};  /* premultiplied */
	auto f = std::make_unique<Flash>();
	f->rect = wlr_scene_rect_create(parent, w, h, colour);
	if (f->rect == nullptr)
		return;
	wlr_scene_node_set_position(&f->rect->node, x, y);
	wlr_scene_node_raise_to_top(&f->rect->node);
	Flash *raw = f.get();
	raw->rect_destroy.connect(&raw->rect->node.events.destroy, [raw](void *) {
		raw->rect = nullptr;  /* its window went first */
		raw->rect_destroy.disconnect();
	});
	raw->timer = wl_event_loop_add_timer(server->wl_event_loop, [](void *data) {
		drop(static_cast<Flash *>(data));  /* removes this timer too */
		return 0;
	}, raw);
	if (raw->timer == nullptr) {
		wlr_scene_node_destroy(&raw->rect->node);
		return;
	}
	wl_event_source_timer_update(raw->timer, kFlashMs);
	state->flashes.push_back(std::move(f));
}

void ring(struct wb_server *server, struct wlr_xdg_system_bell_v1_ring_event *e) {
	struct wb_toplevel *t = nullptr;
	if (e->surface != nullptr)
		t = toplevel_from_surface(server, wlr_surface_get_root_surface(e->surface));
	wlr_log(WLR_INFO, "wb-bell ring %s", t != nullptr ? view_app_id(t) : "(no window)");
	struct wb_config *config = server->config;
	if (config == nullptr)
		return;
	/* Not over the lock screen (nor anything under it). */
	if (config->bell_flash && !session_locked(server)) {
		if (t != nullptr && t->mapped && t->scene_tree != nullptr &&
				t->scene_tree->node.enabled) {
			wb::FrameInsets in = toplevel_insets(t);
			flash(server, t->scene_tree, 0, 0,
					t->geometry.width + in.left + in.right,
					t->geometry.height + in.top + in.bottom);
			wlr_log(WLR_INFO, "wb-bell flash window");
		} else if (struct wlr_output *o = wlr_output_layout_output_at(
				server->output_layout, server->cursor->cursor->x,
				server->cursor->cursor->y)) {
			struct wlr_box box;
			wlr_output_layout_get_box(server->output_layout, o, &box);
			flash(server, &server->scene->tree, box.x, box.y, box.width, box.height);
			wlr_log(WLR_INFO, "wb-bell flash output");
		}
	}
	if (!config->bell_command.empty()) {
		uint32_t now = now_ms();
		if (!state->commanded || now - state->last_command >= kCommandGapMs) {
			state->commanded = true;
			state->last_command = now;
			wb_spawn(config->bell_command.c_str());
		}
	}
}

}  // namespace

void bell_flashes_set_enabled(bool enabled) {
	if (state == nullptr)
		return;
	for (auto &f : state->flashes)
		if (f->rect != nullptr)
			wlr_scene_node_set_enabled(&f->rect->node, enabled);
}

void init_bell(struct wb_server *server) {
	state = new BellState;
	state->bell = wlr_xdg_system_bell_v1_create(server->wl_display, 1);
	if (state->bell == nullptr)
		return;
	state->ring.connect(&state->bell->events.ring, [server](void *data) {
		ring(server, static_cast<struct wlr_xdg_system_bell_v1_ring_event *>(data));
	});
	state->destroy.connect(&state->bell->events.destroy, [](void *) {
		state->ring.disconnect();
		state->destroy.disconnect();
		state->bell = nullptr;
	});
}

void finish_bell(struct wb_server *) {
	if (state == nullptr)
		return;
	while (!state->flashes.empty())
		drop(state->flashes.front().get());
	state->ring.disconnect();
	state->destroy.disconnect();
	delete state;
	state = nullptr;
}
