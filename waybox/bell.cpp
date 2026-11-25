#include "bell.h"

#include <list>
#include <memory>

#include "waybox/wlroots.hpp"
#include "waybox/listener.hpp"
#include "waybox/cursor.h"
#include "waybox/desktop_popup.hpp"
#include "waybox/xdg_shell.h"
#include "config.h"
#include "ipc.h"
#include "script.h"
#include "waybox/session_lock.h"

namespace {

constexpr uint32_t kCommandGapMs = 200;  /* a burst of rings runs it once */

/* A flash on screen: its rectangles in one subtree, shown and hidden as its
 * blinks go, removed after the last (or gone with its window). */
struct Flash {
	struct wlr_scene_tree *tree = nullptr;
	struct wl_event_source *timer = nullptr;
	wb::Listener tree_destroy;
	std::vector<int> toggles;  /* ms from the start: on, off, on, off... */
	size_t next = 0;
	uint32_t start = 0;
	bool visible = false;
};

struct BellState {
	struct wlr_xdg_system_bell_v1 *bell = nullptr;
	wb::Listener ring, destroy;
	std::list<std::unique_ptr<Flash>> flashes;
	std::unique_ptr<wb::DesktopPopup> note;
	wb::BellPacer pacer;
	uint32_t last_command = 0;
	bool commanded = false;
	bool hit_testing = false;  /* flashes off while the pointer's target is found */
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
	f->tree_destroy.disconnect();
	if (f->tree != nullptr)
		wlr_scene_node_destroy(&f->tree->node);
	state->flashes.remove_if([f](const auto &p) { return p.get() == f; });
}

void show(Flash *f, bool visible) {
	f->visible = visible;
	if (f->tree != nullptr)
		wlr_scene_node_set_enabled(&f->tree->node, visible && !state->hit_testing);
}

/* The next on or off, then the timer for the one after (none left: gone). */
int step(void *data) {
	auto *f = static_cast<Flash *>(data);
	if (f->tree == nullptr || f->next >= f->toggles.size()) {
		drop(f);  /* removes this timer too */
		return 0;
	}
	bool on = f->next % 2 == 0;
	show(f, on);
	if (on)
		wlr_log(WLR_INFO, "%s", "wb-bell blink");
	++f->next;
	if (f->next >= f->toggles.size()) {
		drop(f);
		return 0;
	}
	int wait = f->toggles[f->next] - static_cast<int>(now_ms() - f->start);
	wl_event_source_timer_update(f->timer, std::max(1, wait));
	return 0;
}

/* Rectangles at x,y w*h under `parent` in the style's colour: all of it,
 * or (`border`) its edges. */
void flash(struct wb_server *server, struct wlr_scene_tree *parent, int x, int y,
		int w, int h, const wb::BellStyle &style, bool border,
		const std::vector<wb::Blink> &blinks) {
	if (w <= 0 || h <= 0 || blinks.empty())
		return;
	const wb::Color &c = style.color;
	float a = c.a / 255.0f;
	const float colour[4] = {c.r / 255.0f * a, c.g / 255.0f * a, c.b / 255.0f * a, a};
	auto f = std::make_unique<Flash>();
	f->tree = wlr_scene_tree_create(parent);
	if (f->tree == nullptr)
		return;
	wlr_scene_node_set_position(&f->tree->node, x, y);
	wlr_scene_node_raise_to_top(&f->tree->node);
	if (border) {
		int b = std::max(1, std::min({style.width, w / 2, h / 2}));
		const int rects[4][4] = {{0, 0, w, b}, {0, h - b, w, b}, {0, b, b, h - 2 * b},
				{w - b, b, b, h - 2 * b}};
		for (const auto &r : rects)
			if (r[2] > 0 && r[3] > 0) {
				struct wlr_scene_rect *rect = wlr_scene_rect_create(f->tree, r[2], r[3], colour);
				if (rect != nullptr)
					wlr_scene_node_set_position(&rect->node, r[0], r[1]);
			}
	} else {
		wlr_scene_rect_create(f->tree, w, h, colour);
	}
	for (const wb::Blink &bl : blinks) {
		f->toggles.push_back(bl.on_ms);
		f->toggles.push_back(bl.off_ms);
	}
	Flash *raw = f.get();
	raw->tree_destroy.connect(&raw->tree->node.events.destroy, [raw](void *) {
		raw->tree = nullptr;  /* its window went first */
		raw->tree_destroy.disconnect();
	});
	raw->timer = wl_event_loop_add_timer(server->wl_event_loop, step, raw);
	if (raw->timer == nullptr) {
		wlr_scene_node_destroy(&raw->tree->node);
		return;
	}
	raw->start = now_ms();
	state->flashes.push_back(std::move(f));
	step(raw);  /* the first blink, now */
}

struct wlr_output *output_for(struct wb_server *server, struct wb_toplevel *t) {
	if (t != nullptr)
		if (struct wlr_output *o = get_active_output(t))
			return o;
	return wlr_output_layout_output_at(server->output_layout, server->cursor->cursor->x,
			server->cursor->cursor->y);
}

/* Show the bell as `style` says; what was shown, for the log and IPC. */
std::string show_bell(struct wb_server *server, struct wb_toplevel *t,
		const wb::BellStyle &style) {
	std::vector<wb::Blink> blinks = wb::blink_schedule(style);
	if (blinks.empty() || session_locked(server))  /* not over the lock screen */
		return "none";
	if (!state->pacer.allow(now_ms(), blinks)) {
		wlr_log(WLR_INFO, "%s", "wb-bell paced");
		return "paced";
	}
	bool window = t != nullptr && t->mapped && t->scene_tree != nullptr &&
			t->scene_tree->node.enabled;
	if (style.flash == wb::BellFlash::Osd) {
		struct wlr_output *o = output_for(server, t);
		if (o == nullptr)
			return "none";
		wb::Theme theme = server->config != nullptr ? server->config->theme
				: wb::default_theme();
		state->note = std::make_unique<wb::DesktopPopup>(server,
				wb::switcher_style_from_theme(theme));
		std::string text = _("Bell");
		const char *title = t != nullptr ? view_title(t) : nullptr;
		const char *app = t != nullptr ? view_app_id(t) : nullptr;
		if (title != nullptr && *title)
			text += std::string(": ") + title;
		else if (app != nullptr && *app)
			text += std::string(": ") + app;
		state->note->show_on(o, text, style.duration_ms);
		wlr_log(WLR_INFO, "%s", "wb-bell osd");
		return "osd";
	}
	if (window && style.flash != wb::BellFlash::Screen) {
		wb::FrameInsets in = toplevel_insets(t);
		bool border = style.flash == wb::BellFlash::Border;
		flash(server, t->scene_tree, 0, 0, t->geometry.width + in.left + in.right,
				t->geometry.height + in.top + in.bottom, style, border, blinks);
		wlr_log(WLR_INFO, "wb-bell flash %s", border ? "border" : "window");
		return border ? "border" : "window";
	}
	if (struct wlr_output *o = output_for(server, t)) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, o, &box);
		flash(server, &server->scene->tree, box.x, box.y, box.width, box.height, style,
				false, blinks);
		wlr_log(WLR_INFO, "%s", "wb-bell flash output");
		return "screen";
	}
	return "none";
}

void ring_event(struct wb_server *server, struct wlr_xdg_system_bell_v1_ring_event *e) {
	struct wb_toplevel *t = nullptr;
	if (e->surface != nullptr)
		t = toplevel_from_surface(server, wlr_surface_get_root_surface(e->surface));
	bell_ring(server, t, nullptr);
}

}  // namespace

std::string bell_ring(struct wb_server *server, struct wb_toplevel *t, const ScriptBell *preview) {
	if (preview == nullptr)
		wlr_log(WLR_INFO, "wb-bell ring %s",
				t != nullptr && view_app_id(t) ? view_app_id(t) : "(no window)");
	struct wb_config *config = server->config;
	if (config == nullptr || state == nullptr)
		return "none";
	wb::BellStyle style = config->bell_style;
	std::string command = config->bell_command;
	ScriptBell decided;
	const ScriptBell *over = preview;
	/* A Lua "bell" policy decides each real ring (a preview is wbconf's). */
	if (over == nullptr && script_bell(server, t, &decided))
		over = &decided;
	if (over != nullptr) {
		if (over->silent) {
			style.flash = wb::BellFlash::None;
			command.clear();
		} else {
			std::vector<std::string> problems;
			style = wb::bell_style_with(style, over->over, &problems);
			for (const std::string &p : problems)
				wlr_log(WLR_ERROR, "Ignoring the bell setting %s", p.c_str());
			if (over->command)
				command = *over->command;
		}
	}
	std::string shown = show_bell(server, t, style);
	if (!command.empty()) {
		uint32_t now = now_ms();
		if (!state->commanded || now - state->last_command >= kCommandGapMs) {
			state->commanded = true;
			state->last_command = now;
			wb_spawn(command.c_str());
		}
	}
	if (preview == nullptr) {
		script_bell_event(server, t);
		ipc_bell_event(server, t, shown);
	}
	return shown;
}

void bell_flashes_set_enabled(bool enabled) {
	if (state == nullptr)
		return;
	state->hit_testing = !enabled;
	for (auto &f : state->flashes)
		if (f->tree != nullptr)
			wlr_scene_node_set_enabled(&f->tree->node, enabled && f->visible);
}

void init_bell(struct wb_server *server) {
	state = new BellState;
	state->bell = wlr_xdg_system_bell_v1_create(server->wl_display, 1);
	if (state->bell == nullptr)
		return;
	state->ring.connect(&state->bell->events.ring, [server](void *data) {
		ring_event(server, static_cast<struct wlr_xdg_system_bell_v1_ring_event *>(data));
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
	state->note.reset();
	state->ring.disconnect();
	state->destroy.disconnect();
	delete state;
	state = nullptr;
}
