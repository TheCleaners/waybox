/*
 * ext-session-lock-v1 (see include/waybox/session_lock.h for the behaviour).
 */
#include "waybox/session_lock.h"

#include <algorithm>
#include <memory>
#include <set>
#include <vector>

#include "waybox/cursor.h"
#include "waybox/listener.hpp"
#include "waybox/menu_widget.hpp"
#include "waybox/output.h"
#include "waybox/seat.h"
#include "waybox/server.h"
#include "waybox/switcher_widget.hpp"
#include "waybox/xdg_shell.h"

struct wb_session_lock_state;

namespace {

/* Opaque colour shown wherever the locker has not drawn (and over everything
 * if the locker died). */
constexpr float kBlank[4] = {0.08f, 0.08f, 0.08f, 1.0f};

struct LockSurface {
	struct wb_session_lock_state *state = nullptr;
	struct wlr_session_lock_surface_v1 *lock_surface = nullptr;
	/* Our own container, so the locker's content goes away when the lock
	 * surface role is destroyed even if its wl_surface lives on. */
	struct wlr_scene_tree *tree = nullptr;
	wb::Listener map;
	wb::Listener destroy;

	~LockSurface() {
		if (tree != nullptr)
			wlr_scene_node_destroy(&tree->node);
	}
};

}  // namespace

struct wb_session_lock_state {
	struct wb_server *server = nullptr;
	struct wlr_session_lock_manager_v1 *manager = nullptr;
	struct wlr_scene_tree *tree = nullptr;        /* on top of the scene */
	struct wlr_scene_tree *blank_tree = nullptr;  /* child of tree: one rect per output */

	/* Locked from the first lock until an unlock. `lock` is the live locker;
	 * it is null while locked if the locker died without unlocking. */
	bool locked = false;
	struct wlr_session_lock_v1 *lock = nullptr;
	bool locked_sent = false;
	std::set<struct wlr_output *> awaiting_frame;

	std::vector<std::unique_ptr<LockSurface>> surfaces;

	wb::Listener new_lock;
	wb::Listener manager_destroy;
	wb::Listener lock_new_surface;
	wb::Listener lock_unlock;
	wb::Listener lock_destroy;
};

static struct wb_session_lock_state *state_of(const struct wb_server *server) {
	return server ? server->session_lock : nullptr;
}

bool session_locked(const struct wb_server *server) {
	const struct wb_session_lock_state *st = state_of(server);
	return st != nullptr && st->locked;
}

bool session_lock_owns_surface(const struct wb_server *server,
		struct wlr_surface *surface) {
	const struct wb_session_lock_state *st = state_of(server);
	if (st == nullptr || surface == nullptr)
		return false;
	struct wlr_session_lock_surface_v1 *ls =
		wlr_session_lock_surface_v1_try_from_wlr_surface(
				wlr_surface_get_root_surface(surface));
	if (ls == nullptr)
		return false;
	return std::any_of(st->surfaces.begin(), st->surfaces.end(),
			[ls](const std::unique_ptr<LockSurface> &s) {
		return s->lock_surface == ls;
	});
}

void session_lock_keep_on_top(struct wb_server *server) {
	struct wb_session_lock_state *st = state_of(server);
	if (st != nullptr && st->tree != nullptr)
		wlr_scene_node_raise_to_top(&st->tree->node);
}

struct wlr_surface *session_lock_surface_at(struct wb_server *server,
		double lx, double ly, double *sx, double *sy) {
	struct wb_session_lock_state *st = state_of(server);
	if (st == nullptr || !st->locked)
		return nullptr;
	struct wlr_scene_node *node =
		wlr_scene_node_at(&st->tree->node, lx, ly, sx, sy);
	if (node == nullptr || node->type != WLR_SCENE_NODE_BUFFER)
		return nullptr;
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
	return scene_surface ? scene_surface->surface : nullptr;
}

void session_lock_focus(struct wb_server *server) {
	struct wb_session_lock_state *st = state_of(server);
	if (st == nullptr || !st->locked)
		return;
	struct wlr_output *under = wlr_output_layout_output_at(server->output_layout,
			server->cursor->cursor->x, server->cursor->cursor->y);
	LockSurface *pick = nullptr;
	for (auto &s : st->surfaces) {
		if (!s->lock_surface->surface->mapped)
			continue;
		if (pick == nullptr || s->lock_surface->output == under)
			pick = s.get();
	}
	if (pick != nullptr)
		seat_focus_surface(server->seat.get(), pick->lock_surface->surface);
	else
		wlr_seat_keyboard_notify_clear_focus(server->seat->seat);
}

static void position_surface(struct wb_session_lock_state *st, LockSurface *s) {
	struct wlr_output *output = s->lock_surface->output;
	struct wlr_box box;
	wlr_output_layout_get_box(st->server->output_layout, output, &box);
	wlr_scene_node_set_position(&s->tree->node, box.x, box.y);
	int w = 0, h = 0;
	wlr_output_effective_resolution(output, &w, &h);
	wlr_session_lock_surface_v1_configure(s->lock_surface,
			static_cast<uint32_t>(w), static_cast<uint32_t>(h));
}

void session_lock_arrange(struct wb_server *server) {
	struct wb_session_lock_state *st = state_of(server);
	if (st == nullptr || st->blank_tree == nullptr)
		return;
	/* Rebuild the blanking rects to match the current outputs. */
	struct wlr_scene_node *child, *tmp;
	wl_list_for_each_safe(child, tmp, &st->blank_tree->children, link)
		wlr_scene_node_destroy(child);
	struct wb_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (!output->wlr_output->enabled)
			continue;
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
		if (wlr_box_empty(&box))
			continue;
		struct wlr_scene_rect *rect = wlr_scene_rect_create(st->blank_tree,
				box.width, box.height, kBlank);
		wlr_scene_node_set_position(&rect->node, box.x, box.y);
	}
	for (auto &s : st->surfaces)
		position_surface(st, s.get());
}

static void maybe_send_locked(struct wb_session_lock_state *st) {
	if (st->lock != nullptr && !st->locked_sent && st->awaiting_frame.empty()) {
		wlr_session_lock_v1_send_locked(st->lock);
		st->locked_sent = true;
	}
}

void session_lock_output_frame(struct wb_output *output) {
	struct wb_session_lock_state *st = state_of(output->server);
	if (st == nullptr || st->lock == nullptr || st->locked_sent)
		return;
	st->awaiting_frame.erase(output->wlr_output);
	maybe_send_locked(st);
}

/* Drop every interaction that could leak input or content past the lock:
 * open menus and the switcher, grabs, key chains and current focus. */
static void begin_lock(struct wb_session_lock_state *st) {
	struct wb_server *server = st->server;
	server->menu.reset();
	server->switcher.reset();
	server->switcher_windows.clear();
	server->switcher_mods = 0;
	server->active_keychain = nullptr;
	server->frame_pressed = nullptr;
	reset_cursor_mode(server);
	wlr_seat_keyboard_notify_clear_focus(server->seat->seat);
	wlr_seat_pointer_clear_focus(server->seat->seat);

	st->locked = true;
	wlr_scene_node_set_enabled(&st->tree->node, true);
	session_lock_keep_on_top(server);
	session_lock_arrange(server);
}

/* Restore focus to what the user had before locking. */
static void end_lock(struct wb_session_lock_state *st) {
	struct wb_server *server = st->server;
	st->locked = false;
	st->surfaces.clear();
	st->awaiting_frame.clear();
	wlr_scene_node_set_enabled(&st->tree->node, false);

	if (server->seat->focused_layer != nullptr) {
		seat_focus_surface(server->seat.get(),
				server->seat->focused_layer->surface);
		return;
	}
	focus_current_desktop(server);
}

static void handle_new_surface(struct wb_session_lock_state *st,
		struct wlr_session_lock_surface_v1 *lock_surface) {
	auto s = std::make_unique<LockSurface>();
	LockSurface *raw = s.get();
	raw->state = st;
	raw->lock_surface = lock_surface;
	raw->tree = wlr_scene_tree_create(st->tree);
	wlr_scene_subsurface_tree_create(raw->tree, lock_surface->surface);
	position_surface(st, raw);

	raw->map.connect(&lock_surface->surface->events.map, [st](void *) {
		/* Focus the first lock surface to appear (or keep the current one). */
		struct wlr_surface *focused =
			st->server->seat->seat->keyboard_state.focused_surface;
		if (!session_lock_owns_surface(st->server, focused))
			session_lock_focus(st->server);
	});
	raw->destroy.connect(&lock_surface->events.destroy, [st, raw](void *) {
		/* Erasing `raw` destroys this lambda (and its captures) while it
		 * runs, so copy everything needed afterwards first. */
		struct wb_server *server = st->server;
		struct wlr_surface *focused =
			server->seat->seat->keyboard_state.focused_surface;
		bool had_focus = focused != nullptr &&
				wlr_surface_get_root_surface(focused) == raw->lock_surface->surface;
		auto &surfaces = st->surfaces;
		std::erase_if(surfaces, [raw](const std::unique_ptr<LockSurface> &p) {
			return p.get() == raw;
		});
		if (had_focus)
			session_lock_focus(server);
	});
	st->surfaces.push_back(std::move(s));
}

static void handle_new_lock(struct wb_session_lock_state *st,
		struct wlr_session_lock_v1 *lock) {
	if (st->lock != nullptr) {
		/* Already locked by a live locker: refuse (the client gets
		 * "finished"). */
		wlr_session_lock_v1_destroy(lock);
		return;
	}
	bool relock = st->locked;  /* previous locker died while locked */
	st->lock = lock;
	st->locked_sent = false;

	st->lock_new_surface.connect(&lock->events.new_surface, [st](void *data) {
		handle_new_surface(st,
				static_cast<struct wlr_session_lock_surface_v1 *>(data));
	});
	st->lock_unlock.connect(&lock->events.unlock, [st](void *) {
		end_lock(st);
	});
	st->lock_destroy.connect(&lock->events.destroy, [st](void *) {
		/* After an unlock this is just cleanup. Without one, the locker
		 * died: stay locked (blank) and accept a new locker. */
		st->lock_new_surface.disconnect();
		st->lock_unlock.disconnect();
		st->lock_destroy.disconnect();
		st->lock = nullptr;
		st->surfaces.clear();
		st->awaiting_frame.clear();
		if (st->locked) {
			wlr_log(WLR_ERROR, "%s", _("Session locker exited without unlocking; "
					"the session stays locked until a new locker unlocks it"));
			wlr_seat_keyboard_notify_clear_focus(st->server->seat->seat);
		}
	});

	if (relock) {
		/* The outputs are already blank. */
		maybe_send_locked(st);
		return;
	}
	begin_lock(st);
	/* The protocol requires that "locked" is only sent once nothing of the
	 * session is visible: wait for each enabled output to commit a frame with
	 * the blanking rects. */
	struct wb_output *output;
	wl_list_for_each(output, &st->server->outputs, link) {
		if (output->wlr_output->enabled)
			st->awaiting_frame.insert(output->wlr_output);
	}
	maybe_send_locked(st);
}

void init_session_lock(struct wb_server *server) {
	auto *st = new wb_session_lock_state{};
	st->server = server;
	server->session_lock = st;
	st->tree = wlr_scene_tree_create(&server->scene->tree);
	st->blank_tree = wlr_scene_tree_create(st->tree);
	wlr_scene_node_set_enabled(&st->tree->node, false);

	st->manager = wlr_session_lock_manager_v1_create(server->wl_display);
	st->new_lock.connect(&st->manager->events.new_lock, [st](void *data) {
		handle_new_lock(st, static_cast<struct wlr_session_lock_v1 *>(data));
	});
	st->manager_destroy.connect(&st->manager->events.destroy, [st](void *) {
		st->new_lock.disconnect();
		st->manager_destroy.disconnect();
		st->manager = nullptr;
	});
}

void finish_session_lock(struct wb_server *server) {
	struct wb_session_lock_state *st = state_of(server);
	if (st == nullptr)
		return;
	/* The scene (and so our trees) is destroyed separately; only release
	 * listeners and bookkeeping here. */
	for (auto &s : st->surfaces)
		s->tree = nullptr;
	delete st;
	server->session_lock = nullptr;
}
