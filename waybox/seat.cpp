#include <libevdev/libevdev.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <ctime>
#include <optional>
#include <vector>

#include "script.h"
#include "about.h"
#include "waybox/cycle_outline.hpp"
#include "waybox/about_card.hpp"
#include "waybox/confirm_card.hpp"
#include "waybox/geometry_actions.hpp"
#include "color_pick.h"
#include "waybox/stall.hpp"
#include "idle.h"
#include "shortcuts_inhibit.h"
#include "waybox/input_method.h"
#include "waybox/wlroots.hpp"
#if WLR_HAS_LIBINPUT_BACKEND && defined(HAS_LIBINPUT)
#include "waybox/wlroots.hpp"
#else
#undef HAS_LIBINPUT
#endif
#include "waybox/wlroots.hpp"
#include "waybox/wlroots.hpp"
#include "waybox/wlroots.hpp"

#include "waybox/seat.h"
#include "waybox/window_cycle.hpp"
#include "waybox/desktops.hpp"
#include "waybox/client_menu.hpp"
#include "waybox/conditions.hpp"
#include "waybox/xdg_shell.h"
#include "waybox/menu_widget.hpp"
#include "waybox/switcher_widget.hpp"
#include "waybox/style.hpp"
#include "waybox/theme.hpp"
#include "decoration.h"
#include "waybox/session_lock.h"

static void deiconify_toplevel(struct wb_toplevel *toplevel) {
	set_toplevel_minimized(toplevel, false);
}

/* The monitor a desktop or cycling action's <monitor> means (#367):
 * "all" every one (one = nullptr); "this" / "current" the focused window's
 * (else the pointer's); a number (left to right) or a name, that one; not
 * given, "this" with desktops per monitor and "all" with shared ones. */
struct MonitorChoice {
	struct wb_output *one = nullptr;
};

static MonitorChoice action_monitor(struct wb_server *server, const wb::Action &action) {
	auto m = action.args.find("monitor");
	std::string spec = m != action.args.end() ? m->second : "";
	if (spec.empty())
		spec = desktops_per_monitor(server) ? "this" : "all";
	if (spec == "all")
		return {};
	if (spec == "this" || spec == "current")
		return {desktop_action_output(server)};
	if (struct wlr_output *o = rule_output(server, spec))
		return {static_cast<struct wb_output *>(o->data)};
	return {desktop_action_output(server)};
}

/* Whether a window is in the cycle (its monitor's, if one was given). */
static bool on_cycle_monitor(struct wb_server *server, struct wb_toplevel *t) {
	if (server->cycle_monitor.empty())
		return true;
	struct wlr_output *o = get_active_output(t);
	return o != nullptr && server->cycle_monitor == o->name;
}

/* Shared Alt+Tab cycle: walk the MRU focus order to the next/previous eligible
 * toplevel using the pure wb::cycle_next() selector, then focus it. */
static void cycle_toplevels_dir(struct wb_server *server, bool reverse) {
	if (wl_list_empty(&server->focus_order))
		return;

	std::vector<struct wb_toplevel *> order;
	struct wb_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->focus_order, focus_link)
		order.push_back(toplevel);

	struct wlr_surface *focused =
		server->seat->seat->keyboard_state.focused_surface;
	std::optional<std::size_t> current;
	for (std::size_t i = 0; i < order.size(); ++i) {
		if (view_surface(order[i]) == focused) {
			current = i;
			break;
		}
	}

	auto eligible = [&order, server](std::size_t i) {
		return order[i]->mapped && toplevel_on_current_desktop(order[i]) &&
				on_cycle_monitor(server, order[i]) &&
				view_modal_child(order[i]) == nullptr;  /* its modal stands for it */
	};
	std::optional<std::size_t> next =
		wb::cycle_next(order.size(), eligible, current, reverse);
	if (!next)
		return;

	deiconify_toplevel(order[*next]);
	focus_toplevel(order[*next]);
}

static void cycle_toplevels(struct wb_server *server) {
	cycle_toplevels_dir(server, false);
}

static void cycle_toplevels_reverse(struct wb_server *server) {
	cycle_toplevels_dir(server, true);
}

/* The modifier bits whose release commits the Alt+Tab switcher. Shift and the
 * lock modifiers are excluded so Shift+Tab (reverse) and Caps/Num don't end the
 * cycle. */
static constexpr uint32_t kSwitcherTriggerMask =
		WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL;

static std::string toplevel_label(struct wb_toplevel *t) {
	if (view_title(t)[0] != '\0')
		return view_title(t);
	if (view_app_id(t)[0] != '\0')
		return view_app_id(t);
	return "(untitled)";
}

/* The outline around the switcher's selected window (made with the switcher,
 * below its list; gone with it). */
static void outline_selected(struct wb_server *server) {
	if (server->switcher == nullptr)
		return;
	if (server->switcher->outline == nullptr)
		server->switcher->outline = std::make_unique<wb::CycleOutline>(server);
	int sel = server->switcher->selected();
	server->switcher->outline->show(sel >= 0 && sel < static_cast<int>(server->switcher_windows.size())
			? server->switcher_windows[sel] : nullptr);
	server->switcher->raise();
}

/* Dismiss the OSD and focus the selected window (Alt released / Return). */
void wb_switcher_commit(struct wb_server *server) {
	if (server->switcher == nullptr)
		return;
	int sel = server->switcher->selected();
	std::vector<struct wb_toplevel *> wins = std::move(server->switcher_windows);
	server->switcher.reset();
	server->switcher_windows.clear();
	server->switcher_mods = 0;
	if (sel >= 0 && sel < static_cast<int>(wins.size()) && wins[sel]->mapped) {
		deiconify_toplevel(wins[sel]);
		focus_toplevel(wins[sel]);
	}
}

void wb_switcher_forget(struct wb_server *server, struct wb_toplevel *toplevel) {
	if (server->switcher == nullptr)
		return;
	auto &wins = server->switcher_windows;
	if (server->switcher->asked_to_close == toplevel)
		server->switcher->asked_to_close = nullptr;
	auto it = std::find(wins.begin(), wins.end(), toplevel);
	if (it == wins.end())
		return;
	int i = static_cast<int>(it - wins.begin());
	wins.erase(it);
	if (wins.empty()) {
		wb_switcher_cancel(server);
		return;
	}
	server->switcher->remove_entry(i);
	outline_selected(server);
}

static void run_window_action(wb::ActionType type, struct wb_server *server,
		struct wb_toplevel *t);

/* From the switcher: `t` asked to close; asked again (still there), its
 * program ended (Kill, whose own second time forces it); `kill`: ended at
 * once. */
static void switcher_close_window(struct wb_server *server, struct wb_toplevel *t, bool kill) {
	/* Asked again, and it doesn't answer pings: hung, its program goes.
	 * One that answers is only asked again: it may be asking to save. */
	if (!kill && server->switcher->asked_to_close == t && t->not_responding)
		kill = true;
	wlr_log(WLR_INFO, "wb-switcher %s %s", kill ? "kill" : "close",
			view_app_id(t) ? view_app_id(t) : "");
	if (kill) {
		run_window_action(wb::ActionType::Kill, server, t);
	} else {
		view_close(t);
		toplevel_ping(t);  /* hung? (unanswered in 2.5 s: its row says so) */
		server->switcher->asked_to_close = t;
	}
}

void wb_switcher_note(struct wb_server *server, struct wb_toplevel *toplevel) {
	if (server->switcher == nullptr)
		return;
	auto &wins = server->switcher_windows;
	auto it = std::find(wins.begin(), wins.end(), toplevel);
	if (it == wins.end())
		return;
	server->switcher->set_note(static_cast<int>(it - wins.begin()),
			toplevel->not_responding ? _("not responding: close again to end it") : "");
	outline_selected(server);
}

/* Whether `sym` with `modifiers` is the switcher key `spec` (rc.xml's
 * syntax), the switcher's own modifier and the locks left out. */
static bool switcher_key_is(struct wb_server *server, const std::string &spec, xkb_keysym_t sym,
		uint32_t modifiers) {
	if (spec.empty() || spec == "none")
		return false;
	uint32_t want_sym = 0, want_mods = 0;
	wb_parse_key_combo(spec.c_str(), &want_sym, &want_mods);
	if (want_sym == 0)
		return false;
	uint32_t mods = modifiers & ~(server->switcher_mods | WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
	/* Delete and KP_Delete alike. */
	if (sym == XKB_KEY_KP_Delete)
		sym = XKB_KEY_Delete;
	return wb::keysym_fold_case(sym) == wb::keysym_fold_case(want_sym) && mods == want_mods;
}

bool wb_switcher_click(struct wb_server *server, double x, double y, uint32_t button,
		bool pressed) {
	if (server->switcher == nullptr)
		return false;
	wb::SwitcherWidget::Hit h = server->switcher->hit(x, y);
	if (h.row < 0)
		return server->switcher->on_motion(x, y);  /* the panel's edge: taken, nothing */
	/* On press (the release is taken too, as the press was). */
	if (!pressed || button != wb::MOUSE_BUTTON_LEFT)
		return true;
	if (h.row >= static_cast<int>(server->switcher_windows.size()))
		return true;
	struct wb_toplevel *t = server->switcher_windows[h.row];
	if (h.close) {
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat->seat);
		bool kill = kb != nullptr && (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_SHIFT);
		switcher_close_window(server, t, kill);
		return true;
	}
	server->switcher->select(h.row);
	wb_switcher_commit(server);
	return true;
}

/* Dismiss the OSD without changing focus (Escape, or a window vanished). */
void wb_switcher_cancel(struct wb_server *server) {
	server->switcher.reset();
	server->switcher_windows.clear();
	server->switcher_mods = 0;
}

/* Drive the Alt+Tab cycle. If the OSD is already up, advance the selection;
 * otherwise, when a trigger modifier is held, build and show the OSD (commit on
 * release). With no modifier held (e.g. a NextWindow binding without one) fall
 * back to Openbox's instant focus cycle. */
static void switcher_invoke(struct wb_server *server, bool reverse) {
	if (server->switcher != nullptr) {
		if (reverse)
			server->switcher->select_prev();
		else
			server->switcher->select_next();
		outline_selected(server);
		return;
	}

	uint32_t trigger = server->last_key_modifiers & kSwitcherTriggerMask;
	bool osd = server->config == nullptr ? true
			: server->config->switcher_behavior.show_on_hold;
	if (trigger == 0 || !osd) {
		if (reverse)
			cycle_toplevels_reverse(server);
		else
			cycle_toplevels(server);
		return;
	}

	std::vector<struct wb_toplevel *> wins;
	struct wb_toplevel *t;
	wl_list_for_each(t, &server->focus_order, focus_link)
		if (t->mapped && toplevel_on_current_desktop(t) && on_cycle_monitor(server, t))
			wins.push_back(t);
	/* Shown for a single window too: closing it (Delete) is done from
	 * here. With none, nothing. */
	if (wins.empty()) {
		if (reverse)
			cycle_toplevels_reverse(server);
		else
			cycle_toplevels(server);
		return;
	}

	wb::SwitcherBehavior behavior = server->config
			? server->config->switcher_behavior
			: wb::SwitcherBehavior{};

	/* List them in the configured order (<switcher order>): most recently
	 * used, stacking (top to bottom) or spatial (by monitor and position). */
	std::vector<wb::SwitcherEntry> entries;
	entries.reserve(wins.size());
	for (struct wb_toplevel *w : wins) {
		wb::SwitcherEntry e;
		int rank = 0;
		struct wb_toplevel *s;
		wl_list_for_each(s, &server->toplevels, link) {
			if (s == w)
				break;
			++rank;
		}
		e.stack = rank;
		if (struct wlr_output *o = get_active_output(w)) {
			struct wlr_box box;
			wlr_output_layout_get_box(server->output_layout, o, &box);
			e.output_x = box.x;
			e.output_y = box.y;
		}
		e.x = w->geometry.x;
		e.y = w->geometry.y;
		entries.push_back(e);
	}
	std::vector<std::size_t> order = wb::switcher_order(entries, behavior.order);
	std::vector<struct wb_toplevel *> ordered;
	ordered.reserve(wins.size());
	for (std::size_t i : order)
		ordered.push_back(wins[i]);
	/* wins[0] (the head of the focus order) is the current window: the
	 * first Tab selects the one after it in the list, Shift+Tab the one
	 * before (with MRU, the next and the least recently used). */
	int n = static_cast<int>(ordered.size());
	int current = static_cast<int>(std::find(ordered.begin(), ordered.end(), wins[0]) -
			ordered.begin());
	int sel = reverse ? (current - 1 + n) % n : (current + 1) % n;
	/* Another monitor's windows (the focused one isn't among them): the
	 * first Tab selects the most recent of them, not the one after it. */
	if (first_toplevel(server) != wins[0])
		sel = current;
	wins = std::move(ordered);
	wb::SwitcherStyle style = wb_osd_style(server);
	wb::StallScope timing("switcher");
	auto widget = std::make_unique<wb::SwitcherWidget>(server, style, behavior);
	std::vector<wb::SwitcherWidget::Entry> rows;
	rows.reserve(wins.size());
	for (struct wb_toplevel *w : wins)
		rows.push_back({toplevel_label(w), w->icon, ""});
	widget->open(std::move(rows), sel);

	server->switcher_windows = std::move(wins);
	server->switcher_mods = trigger;
	server->switcher = std::move(widget);
	outline_selected(server);
}

/* Open the window menu for the focused window: at the pointer when it is
 * over the window (a titlebar click), otherwise at the window's top-left
 * corner (from the keyboard). Always the native menu: an external launcher
 * can't act on a window. */
/* The window an action acts on: a mouse binding's window while its actions
 * run, else the focused one. */
static struct wb_toplevel *action_target(struct wb_server *server) {
	if (server->action_window != nullptr && toplevel_alive(server, server->action_window))
		return server->action_window;
	return first_toplevel(server);
}

void wb_show_client_menu(struct wb_server *server) {
	if (server->config == nullptr)
		return;  /* no rc.xml loaded: nothing to build the menu from */
	struct wb_toplevel *t = action_target(server);
	if (t == nullptr || !t->scene_tree->node.enabled)
		return;
	wlr_log(WLR_INFO, "wb-menu client-menu %s", view_app_id(t));
	wb::ClientMenuState state;
	for (int i = 0; i < desktop_count(server); i++)
		state.desktop_names.push_back(
				wb::desktop_name(server->config->desktops, i));
	state.desktop = t->desktop;
	state.omnipresent = t->omnipresent;
	state.layer = t->layer;
	state.maximized = t->max_horz && t->max_vert;
	state.shaded = t->shaded;
	state.decorated = t->frame != nullptr;
	state.can_decorate = t->kind == wb_view_kind::Xwayland || t->decoration != nullptr;

	server->menu.reset();  /* it may refer to the previous client_menu */
	server->client_menu = std::make_unique<wb::MenuFile>(wb::build_client_menu(state));
	wb::StallScope timing("menu");
	auto widget = std::make_unique<wb::MenuWidget>(server, *server->client_menu,
			wb::menu_style_from_theme(server->config->theme),
			server->config->menu_behavior);

	double cx = server->cursor->cursor->x, cy = server->cursor->cursor->y;
	wb::FrameInsets in = toplevel_insets(t);
	struct wlr_box frame = {t->geometry.x - in.left, t->geometry.y - in.top,
			t->geometry.width + in.left + in.right,
			t->geometry.height + in.top + in.bottom};
	int x = static_cast<int>(cx), y = static_cast<int>(cy);
	if (!wlr_box_contains_point(&frame, cx, cy)) {
		x = t->geometry.x;
		y = t->geometry.y;
	}
	if (widget->open(wb::kClientMenuId, x, y)) {
		server->menu = std::move(widget);
		server->preview_menu.reset();  /* (the old widget, gone, may have shown it) */
		server->menu_target = t;
	}
}

void wb_run_menu_actions(struct wb_server *server, std::vector<wb::Action> actions) {
	/* The menu that closed was a preview: what was chosen isn't run. */
	if (server->preview_menu != nullptr) {
		server->preview_menu.reset();
		server->menu_target = nullptr;
		return;
	}
	struct wb_toplevel *target = server->menu_target;
	server->menu_target = nullptr;
	if (target != nullptr) {
		/* Act on the window the menu was opened for. */
		if (!toplevel_alive(server, target))
			return;
		focus_toplevel(target);
	}
	wb::run_actions(actions, server);
}

bool wb::run_actions(const std::vector<wb::Action> &actions, struct wb_server *server) {
	const struct wb_config *config = server->config;
	for (const wb::Action &action : actions) {
		if (action.type == wb::ActionType::Stop)
			return true;
		wb::run_action(action, server);
		if (server->config != config)
			return true;
	}
	return false;
}

/* ---- If, ForEach (Openbox's conditional actions; conditions.hpp) ---- */

namespace {

int conditional_depth = 0;  /* If/ForEach within If/ForEach, bounded */

/* What a condition can ask about a window. */
wb::WindowFacts window_facts(struct wb_server *server, struct wb_toplevel *t) {
	wb::WindowFacts f;
	f.title = view_title(t) ? view_title(t) : "";
	f.app_id = view_app_id(t) ? view_app_id(t) : "";
	f.tag = t->tag;
	f.type = view_parent(t) != nullptr ? "dialog" : "normal";
	f.shaded = t->shaded;
	f.maximized_horizontal = t->max_horz;
	f.maximized_vertical = t->max_vert;
	f.iconified = t->minimized;
	f.fullscreen = t->fullscreen;
	struct wlr_surface *s = view_surface(t);
	f.focused = s != nullptr && server->seat->seat->keyboard_state.focused_surface == s;
	f.focusable = t->mapped;
	f.urgent = t->urgent;
	f.decorated = t->frame != nullptr;
	f.omnipresent = t->omnipresent;
	f.desktop = t->desktop;
	/* Monitors are numbered from 1, in the order IPC lists them. */
	struct wlr_output *output = get_active_output(t);
	int n = 0;
	struct wb_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		++n;
		if (o->wlr_output == output) {
			f.monitor = n;
			break;
		}
	}
	return f;
}

struct wb_toplevel *keyboard_focused(struct wb_server *server) {
	struct wlr_surface *focused = server->seat->seat->keyboard_state.focused_surface;
	struct wb_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link)
		if (t->mapped && focused != nullptr && view_surface(t) == focused)
			return t;
	return nullptr;
}

bool conditions_hold(struct wb_server *server, const wb::Conditional &c,
		struct wb_toplevel *target) {
	std::optional<wb::WindowFacts> action_facts, focus_facts;
	if (target != nullptr)
		action_facts = window_facts(server, target);
	if (struct wb_toplevel *f = keyboard_focused(server))
		focus_facts = window_facts(server, f);
	wb::DesktopFacts d{0, 0};
	desktop_action_state(server, &d.current, &d.last);
	return wb::conditions_true(c.queries, action_facts ? &*action_facts : nullptr,
			focus_facts ? &*focus_facts : nullptr, d);
}

/* Run `actions` for `window` (the window they act on). */
bool run_for(struct wb_server *server, const std::vector<wb::Action> &actions,
		struct wb_toplevel *window) {
	struct wb_toplevel *saved = server->action_window;
	server->action_window = window;
	bool stopped = wb::run_actions(actions, server);
	server->action_window = toplevel_alive(server, saved) ? saved : nullptr;
	return stopped;
}

}  // namespace

static struct wb_toplevel *action_target(struct wb_server *server);

/* If: the branch its conditions choose, on the action's window. ForEach:
 * that for every window, oldest first, until a Stop; <none> if no window
 * matched. Like any other action, they never end their own list (a Stop
 * inside a branch ends that branch, and a ForEach loop). */
static void run_conditional(const wb::Action &action, struct wb_server *server) {
	const wb::Conditional *c = action.conditional.get();
	if (c == nullptr || conditional_depth >= 16)
		return;
	++conditional_depth;
	struct wb_toplevel *target = action_target(server);
	if (action.type == wb::ActionType::If) {
		bool yes = conditions_hold(server, *c, target);
		run_for(server, yes ? c->then_actions : c->else_actions, target);
	} else {
		/* A snapshot: the actions may open, close or restack windows. */
		std::vector<struct wb_toplevel *> windows;
		struct wb_toplevel *t;
		wl_list_for_each_reverse(t, &server->toplevels, link)
			if (t->mapped)
				windows.push_back(t);
		bool any = false;
		for (struct wb_toplevel *w : windows) {
			if (!toplevel_alive(server, w) || !w->mapped)
				continue;
			bool yes = conditions_hold(server, *c, w);
			any = any || yes;
			if (run_for(server, yes ? c->then_actions : c->else_actions, w))
				break;  /* a Stop ends the loop */
		}
		if (!any)
			run_for(server, c->none_actions,
					toplevel_alive(server, target) ? target : nullptr);
	}
	--conditional_depth;
}

/* Run a single parsed action against the live compositor state. This is the
 * "live" half of the action framework (the registry/parsing half lives in
 * action.cpp). Adding a new action means adding a case here plus a row in the
 * action.cpp registry. */
/* Openbox's geometry actions on window `t`, in the usable area of its
 * monitor (or <monitor>N</monitor>'s, for MoveResizeTo); the edge ones stop
 * at the other windows shown on this desktop. */
static void run_geometry_action(const wb::Action &action, struct wb_server *server,
		struct wb_toplevel *t) {
	using T = wb::ActionType;
	struct wlr_output *output = get_active_output(t);
	if (action.type == T::MoveResizeTo) {
		/* As window rules count them: left to right (not the order
		 * they were plugged in), or by name, mouse, active, primary. */
		auto m = action.args.find("monitor");
		if (m != action.args.end() && m->second != "current")
			if (struct wlr_output *o = rule_output(server, m->second))
				output = o;
	}
	if (output == nullptr)
		return;
	struct wlr_box ua = output_usable_area(server, output);
	wb::Rect area{ua.x, ua.y, ua.width, ua.height};
	wb::Rect frame = toplevel_frame(t);
	std::vector<wb::Rect> others;
	struct wb_toplevel *o;
	wl_list_for_each(o, &server->toplevels, link)
		if (o != t && o->mapped && !o->minimized && o->scene_tree != nullptr &&
				o->scene_tree->node.enabled && toplevel_on_current_desktop(o))
			others.push_back(toplevel_frame(o));
	auto dir = wb::edge_dir_from_name(action.direction);
	wb::Rect r = frame;
	switch (action.type) {
	case T::MoveResizeTo:
		r = wb::move_resize_to(frame, area, action.args);
		break;
	case T::MoveRelative:
		r = wb::move_relative(frame, action.args);
		break;
	case T::ResizeRelative:
		r = wb::resize_relative(frame, action.args);
		break;
	case T::MoveToCenter:
		r = wb::move_to_center(frame, area);
		break;
	case T::MoveToEdge:
		if (dir)
			r = wb::move_to_edge(frame, area, others, *dir);
		break;
	case T::GrowToEdge:
		if (dir)
			r = wb::grow_to_edge(frame, area, others, *dir);
		break;
	case T::ShrinkToEdge:
		if (dir)
			r = wb::shrink_to_edge(frame, others, *dir);
		break;
	case T::GrowToFill:
		r = wb::grow_to_fill(frame, area, others);
		break;
	default:
		break;
	}
	if (r.x != frame.x || r.y != frame.y || r.width != frame.width || r.height != frame.height)
		set_toplevel_frame(t, r);
}

/* Openbox's smaller window actions. */
static void run_window_action(wb::ActionType type, struct wb_server *server,
		struct wb_toplevel *t) {
	using T = wb::ActionType;
	switch (type) {
	case T::Kill: {
		/* X11: the process ID is the program's own say (_NET_WM_PID), so
		 * a wrong one could name another of the user's programs: asked to
		 * close instead, no signal. */
		if (t->kind == wb_view_kind::Xwayland) {
			wlr_log(WLR_INFO, "%s", "wb-kill x11: asked to close");
			view_close(t);
			break;
		}
		/* Wayland: the connection's own credentials. Only a process of the
		 * user's, never Waybox or init. */
		uid_t uid;
		pid_t pid = view_pid(t, &uid);
		if (pid <= 1 || pid == getpid() || uid != getuid())
			break;
		timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		int64_t now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
		bool again = server->kill_pid == pid && now - server->kill_ms < 3000;
		kill(pid, again ? SIGKILL : SIGTERM);
		wlr_log(WLR_INFO, "wb-kill %d %s", pid, again ? "KILL" : "TERM");
		server->kill_pid = again ? -1 : pid;
		server->kill_ms = now;
		break;
	}
	case T::Decorate:
	case T::Undecorate:
		t->decorations = type == T::Decorate ? wb::DecorMode::Full : wb::DecorMode::None;
		apply_toplevel_decoration(t);
		update_toplevel_decoration(t);
		break;
	case T::RaiseLower: {
		/* On top of its layer already: lowered; else raised. */
		struct wlr_scene_node *n = &t->scene_tree->node;
		bool on_top = n->parent != nullptr && n->link.next == &n->parent->children;
		if (on_top)
			lower_toplevel(t);
		else
			raise_toplevel(t);
		break;
	}
	case T::ShadeLower:
		if (!t->shaded)
			set_toplevel_shaded(t, true);
		else
			lower_toplevel(t);
		break;
	case T::UnshadeRaise:
		if (t->shaded)
			set_toplevel_shaded(t, false);
		else
			raise_toplevel(t);
		break;
	case T::FocusToBottom:
		focus_order_to_bottom(t);
		break;
	case T::Unfocus:
		unfocus_toplevel(t);
		break;
	default:
		break;
	}
}

void wb::run_action(const wb::Action &action, struct wb_server *server) {
	switch (action.type) {
	case wb::ActionType::Execute:
		wb_spawn(action.command.c_str());
		break;
	case wb::ActionType::NextWindow:
	case wb::ActionType::PreviousWindow:
		/* A cycle on its way keeps the monitor it started with. */
		if (server->switcher == nullptr) {
			struct wb_output *one = action_monitor(server, action).one;
			server->cycle_monitor = one != nullptr ? one->wlr_output->name : "";
		}
		switcher_invoke(server, action.type == wb::ActionType::PreviousWindow);
		break;
	case wb::ActionType::Close: {
		struct wb_toplevel *toplevel = action_target(server);
		if (toplevel && toplevel->scene_tree->node.enabled)
			view_close(toplevel);
		break;
	}
	case wb::ActionType::Maximize:
	case wb::ActionType::Unmaximize:
	case wb::ActionType::ToggleMaximize: {
		/* As Openbox: <direction> both (default), horizontal (horz) or
		 * vertical (vert); a toggle in one direction looks only at that
		 * direction, a toggle of both at whether it's fully maximized. */
		struct wb_toplevel *toplevel = action_target(server);
		if (toplevel == nullptr || !toplevel->scene_tree->node.enabled)
			break;
		const std::string &d = action.direction;
		bool horz = d == "horizontal" || d == "horz";
		bool vert = d == "vertical" || d == "vert";
		bool on = action.type == wb::ActionType::Maximize;
		if (action.type == wb::ActionType::ToggleMaximize)
			on = horz ? !toplevel->max_horz : vert ? !toplevel->max_vert
					: !(toplevel->max_horz && toplevel->max_vert);
		bool h = horz || !vert ? on : toplevel->max_horz;
		bool v = vert || !horz ? on : toplevel->max_vert;
		set_toplevel_maximized(toplevel, h, v);
		break;
	}
	case wb::ActionType::Fullscreen: {
		struct wb_toplevel *toplevel = action_target(server);
		if (toplevel && toplevel->scene_tree->node.enabled)
			set_toplevel_fullscreen(toplevel,
					!toplevel->fullscreen);
		break;
	}
	case wb::ActionType::Iconify: {
		struct wb_toplevel *toplevel = action_target(server);
		if (toplevel && toplevel->scene_tree->node.enabled) {
			set_toplevel_minimized(toplevel, true);
		}
		break;
	}
	case wb::ActionType::ToggleShade: {
		struct wb_toplevel *toplevel = action_target(server);
		if (toplevel && toplevel->scene_tree->node.enabled)
			set_toplevel_shaded(toplevel, !toplevel->shaded);
		break;
	}
	case wb::ActionType::Shade:
	case wb::ActionType::Unshade: {
		struct wb_toplevel *toplevel = action_target(server);
		if (toplevel && toplevel->scene_tree->node.enabled)
			set_toplevel_shaded(toplevel, action.type == wb::ActionType::Shade);
		break;
	}
	case wb::ActionType::Focus: {
		/* Openbox's Focus: keyboard focus to the window. focus_toplevel()
		 * raises it, brings it back if minimized, and shows its desktop. */
		struct wb_toplevel *toplevel = action_target(server);
		if (toplevel != nullptr && toplevel->mapped)
			focus_toplevel(toplevel);
		break;
	}
	case wb::ActionType::Raise: {
		struct wb_toplevel *toplevel = action_target(server);
		if (toplevel && toplevel->scene_tree->node.enabled)
			raise_toplevel(toplevel);
		break;
	}
	case wb::ActionType::Lower: {
		struct wb_toplevel *toplevel = action_target(server);
		if (toplevel && toplevel->scene_tree->node.enabled)
			lower_toplevel(toplevel);
		break;
	}
	case wb::ActionType::Reconfigure:
		wb_reconfigure(server);
		break;
	case wb::ActionType::ToggleDecorations: {
		struct wb_toplevel *toplevel = action_target(server);
		if (toplevel && toplevel->scene_tree->node.enabled) {
			toplevel->decorations = wb::toggle_decor(toplevel_wants_ssd(toplevel));
			apply_toplevel_decoration(toplevel);
		}
		break;
	}
	case wb::ActionType::ShowMenu: {
		if (server->config == nullptr)
			break;
		if (action.command == wb::kClientMenuId) {
			wb_show_client_menu(server);
			break;
		}
		/* Delegate to an external launcher when configured (built-in-or-delegate
		 * pillar); otherwise open the native menu widget. */
		if (server->config->menu_source.kind ==
				wb::MenuSource::Kind::External) {
			wb_spawn(server->config->menu_source.command.c_str());
			break;
		}
		wb::StallScope timing("menu");
		auto widget = std::make_unique<wb::MenuWidget>(server,
				server->config->menu,
				wb::menu_style_from_theme(server->config->theme),
				server->config->menu_behavior);
		int x = static_cast<int>(server->cursor->cursor->x);
		int y = static_cast<int>(server->cursor->cursor->y);
		/* <position>: x and y both (as Openbox), on <monitor> (the
		 * primary if none; "all": the whole layout); else at the pointer. */
		wb::MenuWidget::Placement placement;
		auto px = action.args.find("position.x"), py = action.args.find("position.y");
		if (px != action.args.end() && py != action.args.end()) {
			auto pm = action.args.find("position.monitor");
			std::string monitor = pm != action.args.end() ? pm->second : "primary";
			struct wlr_box area = {};
			if (monitor == "all") {
				wlr_output_layout_get_box(server->output_layout, nullptr, &area);
			} else {
				struct wlr_output *out = rule_output(server, monitor);
				if (out == nullptr)
					out = rule_output(server, "primary");
				if (out != nullptr)
					wlr_output_layout_get_box(server->output_layout, out, &area);
			}
			placement = {px->second, py->second, area.x, area.y, area.width, area.height};
		}
		if (widget->open(action.command, x, y, placement)) {
			server->menu = std::move(widget);
		server->preview_menu.reset();  /* (the old widget, gone, may have shown it) */
			server->menu_target = nullptr;
		}
		break;
	}
	case wb::ActionType::If:
	case wb::ActionType::ForEach:
		run_conditional(action, server);
		break;
	case wb::ActionType::Stop:
		break;  /* (it ends a list: run_actions) */
	case wb::ActionType::ScriptCall:
		script_menu_call(server, action.script_callback);
		break;
	case wb::ActionType::GoToDesktop: {
		/* Desktops per monitor: the monitor <monitor> names (the focused
		 * window's, else the pointer's, by default), or every one ("all"),
		 * counting from that one's (or the focused one's) desktop. */
		MonitorChoice m = action_monitor(server, action);
		int cur = 0, last = 0;
		if (m.one != nullptr && desktops_per_monitor(server)) {
			cur = m.one->desktop;
			last = m.one->last_desktop;
		} else {
			desktop_action_state(server, &cur, &last);
		}
		if (auto target = wb::desktop_target(action.to, cur, last,
				desktop_count(server), action.wrap)) {
			if (!desktops_per_monitor(server))
				wb_go_to_desktop(server, *target);
			else if (m.one == nullptr)
				wb_go_to_desktop_everywhere(server, *target);
			else
				wb_go_to_desktop_on(server, m.one, *target);
		}
		break;
	}
	case wb::ActionType::SendToDesktop: {
		/* Any mapped window, minimized or on another desktop too (as in
		 * Openbox): only where it lives changes. */
		struct wb_toplevel *toplevel = action_target(server);
		if (toplevel == nullptr || !toplevel->mapped)
			break;
		int cur = 0, last = 0;
		desktop_action_state(server, &cur, &last);
		if (auto target = wb::desktop_target(action.to, cur, last,
				desktop_count(server), action.wrap))
			wb_send_to_desktop(toplevel, *target, action.follow);
		break;
	}
	case wb::ActionType::SendToLayer:
	case wb::ActionType::ToggleAlwaysOnTop:
	case wb::ActionType::ToggleAlwaysOnBottom: {
		struct wb_toplevel *toplevel = action_target(server);
		if (toplevel == nullptr || !toplevel->mapped)
			break;
		int layer = 0;
		if (action.type == wb::ActionType::ToggleAlwaysOnTop)
			layer = toplevel->layer == 1 ? 0 : 1;
		else if (action.type == wb::ActionType::ToggleAlwaysOnBottom)
			layer = toplevel->layer == -1 ? 0 : -1;
		else
			layer = action.direction == "top" ? 1 : action.direction == "bottom" ? -1 : 0;
		set_toplevel_layer(toplevel, layer);
		break;
	}
	case wb::ActionType::MoveResizeTo:
	case wb::ActionType::MoveRelative:
	case wb::ActionType::ResizeRelative:
	case wb::ActionType::MoveToCenter:
	case wb::ActionType::MoveToEdge:
	case wb::ActionType::GrowToEdge:
	case wb::ActionType::ShrinkToEdge:
	case wb::ActionType::GrowToFill: {
		struct wb_toplevel *t = action_target(server);
		if (t == nullptr || !t->mapped || !t->scene_tree->node.enabled)
			break;
		run_geometry_action(action, server, t);
		break;
	}
	case wb::ActionType::DirectionalFocus: {
		/* From the focused window (else the pointer) to the nearest window
		 * that way, among those shown on this desktop. */
		auto dir = wb::dir8_from_name(action.direction);
		if (!dir)
			break;
		struct wb_toplevel *from = action_target(server);
		wb::Rect origin = from != nullptr && from->mapped
				? toplevel_frame(from)
				: wb::Rect{static_cast<int>(server->cursor->cursor->x),
						static_cast<int>(server->cursor->cursor->y), 1, 1};
		std::vector<struct wb_toplevel *> wins;
		std::vector<wb::Rect> rects;
		struct wb_toplevel *o;
		wl_list_for_each(o, &server->toplevels, link)
			if (o != from && o->mapped && !o->minimized && o->scene_tree != nullptr &&
					o->scene_tree->node.enabled && toplevel_on_current_desktop(o)) {
				wins.push_back(o);
				rects.push_back(toplevel_frame(o));
			}
		if (auto i = wb::directional_target(origin, rects, *dir)) {
			focus_toplevel(wins[*i]);
			raise_toplevel(wins[*i]);
		}
		break;
	}
	case wb::ActionType::Kill:
	case wb::ActionType::Decorate:
	case wb::ActionType::Undecorate:
	case wb::ActionType::RaiseLower:
	case wb::ActionType::ShadeLower:
	case wb::ActionType::UnshadeRaise:
	case wb::ActionType::FocusToBottom:
	case wb::ActionType::Unfocus: {
		struct wb_toplevel *t = action_target(server);
		if (t == nullptr || !t->mapped)
			break;
		run_window_action(action.type, server, t);
		break;
	}
	case wb::ActionType::AddDesktop:
		add_desktop(server, action.direction == "current");
		break;
	case wb::ActionType::RemoveDesktop:
		remove_desktop(server, action.direction == "current");
		break;
	case wb::ActionType::FocusFallback: {
		struct wb_toplevel *t;
		wl_list_for_each(t, &server->focus_order, focus_link)
			if (t->mapped && !t->minimized && toplevel_on_current_desktop(t)) {
				focus_toplevel(t);
				break;
			}
		break;
	}
	case wb::ActionType::ToggleShowDesktop:
		toggle_show_desktop(server, action_monitor(server, action).one);
		break;
	case wb::ActionType::ShowAbout:
		/* Nothing drawn over the lock screen. */
		if (session_locked(server))
			break;
		server->menu.reset();
		server->about_card = std::make_unique<wb::AboutCard>(server, wb_osd_style(server),
				about_info(server));
		break;
	case wb::ActionType::ToggleOmnipresent: {
		struct wb_toplevel *toplevel = action_target(server);
		if (toplevel && toplevel->mapped)
			wb_toggle_omnipresent(toplevel);
		break;
	}
	case wb::ActionType::SnapToEdge: {
		struct wb_toplevel *toplevel = action_target(server);
		if (toplevel == nullptr || !toplevel->scene_tree->node.enabled)
			break;
		if (action.direction == "down") {
			/* Back to how it was before tiling or maximizing. */
			if (toplevel->tiled)
				untile_toplevel(toplevel, true);
			else if (toplevel->max_horz || toplevel->max_vert)
				set_toplevel_maximized(toplevel, false, false);
		} else if (auto zone = wb::tile_zone_from_name(action.direction)) {
			tile_toplevel(toplevel, *zone, nullptr);
		}
		break;
	}
	case wb::ActionType::Move:
	case wb::ActionType::Resize: {
		/* Start moving or resizing with the pointer, as a titlebar or
		 * border drag does (resizing from the nearest corner). */
		struct wb_toplevel *toplevel = action_target(server);
		if (toplevel == nullptr || !toplevel->scene_tree->node.enabled ||
				toplevel->fullscreen)
			break;
		if (action.type == wb::ActionType::Move)
			begin_interactive(toplevel, WB_CURSOR_MOVE, 0);
		else
			begin_interactive(toplevel, WB_CURSOR_RESIZE,
					nearest_resize_edges(toplevel, server->cursor->cursor->x,
							server->cursor->cursor->y));
		break;
	}
	case wb::ActionType::Exit: {
		/* As Openbox: asked first (<prompt>no</prompt>: not). */
		auto p = action.args.find("prompt");
		if (p != action.args.end() && (p->second == "no" || p->second == "false" ||
				p->second == "off" || p->second == "0")) {
			wl_display_terminate(server->wl_display);
			break;
		}
		server->confirm = std::make_unique<wb::ConfirmCard>(server, wb_osd_style(server),
				_("Exit Waybox?"), _("Programs still open will close."), _("Exit"),
				_("Cancel"));
		break;
	}
	}
}

/* A lone modifier keypress must never advance or abort a key chain (e.g.
 * releasing and re-pressing Super between the two keys of "W-a, b"). */
static bool is_modifier_keysym(xkb_keysym_t sym) {
	switch (sym) {
	case XKB_KEY_Shift_L:
	case XKB_KEY_Shift_R:
	case XKB_KEY_Control_L:
	case XKB_KEY_Control_R:
	case XKB_KEY_Alt_L:
	case XKB_KEY_Alt_R:
	case XKB_KEY_Super_L:
	case XKB_KEY_Super_R:
	case XKB_KEY_Meta_L:
	case XKB_KEY_Meta_R:
	case XKB_KEY_Hyper_L:
	case XKB_KEY_Hyper_R:
	case XKB_KEY_Caps_Lock:
	case XKB_KEY_Num_Lock:
	case XKB_KEY_ISO_Level3_Shift:
	case XKB_KEY_ISO_Level5_Shift:
		return true;
	default:
		return false;
	}
}

static bool handle_keybinding(struct wb_server *server, xkb_keysym_t sym, uint32_t modifiers,
		std::vector<wb::Action> *repeat_actions) {
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 *
	 * Returns true if the keybinding is handled, false to send it to the
	 * client.
	 */

	/* Ignore modifier keys entirely so they don't disturb an in-progress
	 * key chain. */
	if (is_modifier_keysym(sym))
		return false;

	/* TODO: Make these configurable through rc.xml */
	if (modifiers & (WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT) &&
		sym >= XKB_KEY_XF86Switch_VT_1 &&
		sym <= XKB_KEY_XF86Switch_VT_12) {
		unsigned int vt = sym - XKB_KEY_XF86Switch_VT_1 + 1;
		wlr_session_change_vt (server->session, vt);

		return true;
	}

	if (!server->config) {
		/* Some default key bindings, when the rc.xml file can't be
		 * parsed. */
		if (modifiers & WLR_MODIFIER_ALT && sym == XKB_KEY_Tab)
			cycle_toplevels(server);
		else if (modifiers & (WLR_MODIFIER_ALT|WLR_MODIFIER_SHIFT) &&
				sym == XKB_KEY_Tab)
			cycle_toplevels_reverse(server);
		else if (sym == XKB_KEY_Escape && modifiers & WLR_MODIFIER_CTRL)
			wl_display_terminate(server->wl_display);
		else
			return false;
		return true;
	}

	/* Keys scripts bound (wb.bind) come before rc.xml's. */
	if (server->active_keychain == nullptr && script_key_binding(server, sym, modifiers))
		return true;

	/* Resolve against the key-binding tree, tracking chain state. While a chain
	 * is active we match against the current node's children, otherwise the
	 * root bindings. */
	const std::vector<wb::KeyBinding> &children =
		server->active_keychain ? server->active_keychain->children
		                        : server->config->key_bindings;
	wb::ChainStep step = wb::chain_step(children, sym, modifiers);
	switch (step.outcome) {
	case wb::ChainOutcome::Fire: {
		/* Copy the actions before running them: a Reconfigure action rebuilds
		 * the binding tree and would free step.node mid-iteration. */
		std::vector<wb::Action> actions = step.node->actions;
		if (step.node->repeat && repeat_actions != nullptr)
			*repeat_actions = actions;
		server->active_keychain = nullptr;
		/* Expose the triggering modifiers so NextWindow/PreviousWindow can tell
		 * an Alt-held interactive cycle from a modifier-less instant switch. */
		server->last_key_modifiers = modifiers;
		wb::run_actions(actions, server);
		return true;
	}
	case wb::ChainOutcome::Descend:
		server->active_keychain = step.node;
		return true;
	case wb::ChainOutcome::NoMatch:
		if (server->active_keychain != nullptr) {
			/* An unmatched key aborts the in-progress chain and is consumed. */
			server->active_keychain = nullptr;
			return true;
		}
		return false;
	}
	return false;
}

/* Marks devices created through the virtual input protocols (wlroots has a
 * getter for virtual keyboards but none for virtual pointers). */
static char virtual_device_tag;

bool wb_input_is_virtual(struct wlr_input_device *device) {
	return device != nullptr && device->data == &virtual_device_tag;
}

bool wb_input_blocked(struct wb_server *server, struct wlr_input_device *device) {
	if (!wb_input_is_virtual(device))
		return false;
	auto policy = server->config ? server->config->virtual_input
			: wb_config::VirtualInput::Unlocked;
	switch (policy) {
	case wb_config::VirtualInput::Always:
		return false;
	case wb_config::VirtualInput::Never:
		return true;
	case wb_config::VirtualInput::Unlocked:
	default:
		return session_locked(server);
	}
}

void wb_keyboard::on_modifiers(void *data) {
	/* Raised when a modifier key (shift, alt, ...) is pressed; forward it.
	 *
	 * A seat can only have one keyboard, but that is a Wayland-protocol
	 * limitation, not a wlroots one: we assign every connected keyboard to the
	 * same seat and wlr_seat swaps the underlying wlr_keyboard transparently. */
	if (wb_input_blocked(server, &keyboard->base))
		return;
	/* An input method with a keyboard grab gets the modifiers instead. */
	if (session_locked(server) || !input_method_modifiers(server, keyboard)) {
		wlr_seat_set_keyboard(server->seat->seat, keyboard);
		wlr_seat_keyboard_notify_modifiers(server->seat->seat,
			&keyboard->modifiers);
	}

	/* Committing the Alt+Tab cycle: once any of the modifiers that opened the
	 * switcher is released, focus the selected window and drop the OSD. */
	if (server->switcher != nullptr && server->switcher_mods != 0) {
		uint32_t mods = wlr_keyboard_get_modifiers(keyboard);
		if ((mods & server->switcher_mods) != server->switcher_mods)
			wb_switcher_commit(server);
	}
}

void wb_keyboard::on_key(void *data) {
	/* Raised when a key is pressed or released. */
	auto *event = static_cast<struct wlr_keyboard_key_event *>(data);
	struct wlr_seat *seat = server->seat->seat;

	if (wb_input_blocked(server, &keyboard->base))
		return;
	/* Any key event ends a binding's repeat: its key let go, another
	 * pressed, a modifier changed. */
	stop_repeat();

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard);

	/* Locked: no compositor bindings, menus or switcher. Keys go only to the
	 * focused surface, which can only be a lock surface; VT switching still
	 * works so a stuck lock can be recovered from another VT. */
	if (session_locked(server)) {
		if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED &&
				(modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) &&
				server->session != nullptr) {
			for (int i = 0; i < nsyms; i++) {
				if (syms[i] >= XKB_KEY_XF86Switch_VT_1 &&
						syms[i] <= XKB_KEY_XF86Switch_VT_12) {
					wlr_session_change_vt(server->session,
							syms[i] - XKB_KEY_XF86Switch_VT_1 + 1);
					return;
				}
			}
		}
		wlr_seat_set_keyboard(seat, keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
		idle_activity(server, seat);
		return;
	}

	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* Escape ends a color pick (and goes no further). */
		for (int i = 0; i < nsyms; i++)
			if (color_pick_key(server, syms[i]))
				return;
		/* The Alt+Tab OSD grabs the keyboard: Tab/Shift+Tab move the selection,
		 * Escape cancels, Return commits. All keys are swallowed so they neither
		 * fire bindings nor reach clients while cycling. */
		if (server->switcher != nullptr) {
			for (int i = 0; i < nsyms; i++) {
				xkb_keysym_t s = syms[i];
				if (s == XKB_KEY_Escape) {
					wb_switcher_cancel(server);
					break;
				}
				if (s == XKB_KEY_Return || s == XKB_KEY_KP_Enter) {
					wb_switcher_commit(server);
					break;
				}
				/* Its keys (<switcher closeKey killKey>, Delete and
				 * Shift+Delete): the selected window asked to close (again:
				 * its program ended), or its program ended. The switcher
				 * stays: the row goes when the window does. */
				if (server->switcher->can_close()) {
					const wb::SwitcherBehavior &b = server->switcher->behavior();
					bool kill = switcher_key_is(server, b.kill_key, s, modifiers);
					if (kill || switcher_key_is(server, b.close_key, s, modifiers)) {
						int sel = server->switcher->selected();
						if (sel >= 0 && sel < static_cast<int>(server->switcher_windows.size()))
							switcher_close_window(server, server->switcher_windows[sel], kill);
						break;
					}
				}
				if (s == XKB_KEY_Tab || s == XKB_KEY_ISO_Left_Tab) {
					bool reverse = (s == XKB_KEY_ISO_Left_Tab) ||
							(modifiers & WLR_MODIFIER_SHIFT);
					if (reverse)
						server->switcher->select_prev();
					else
						server->switcher->select_next();
					outline_selected(server);
				} else if (s == XKB_KEY_Up || s == XKB_KEY_Left) {
					server->switcher->select_prev();
					outline_selected(server);
				} else if (s == XKB_KEY_Down || s == XKB_KEY_Right) {
					server->switcher->select_next();
					outline_selected(server);
				}
			}
			idle_activity(server, seat);
			return;
		}

		/* A question asked (Exit's prompt): its keys answer it, and the
		 * keyboard goes nowhere else meanwhile. */
		if (server->confirm != nullptr) {
			for (int i = 0; i < nsyms; i++) {
				wb::ConfirmCard::Answer a = server->confirm->on_key(syms[i]);
				if (a == wb::ConfirmCard::Answer::None)
					continue;
				server->confirm.reset();
				wlr_log(WLR_INFO, "wb-confirm %s", a == wb::ConfirmCard::Answer::Yes ? "yes" : "no");
				if (a == wb::ConfirmCard::Answer::Yes)
					wl_display_terminate(server->wl_display);
				break;
			}
			idle_activity(server, seat);
			return;
		}

		/* The About card: any key closes it, and goes no further. */
		if (server->about_card != nullptr) {
			server->about_card.reset();
			idle_activity(server, seat);
			return;
		}

		/* An open menu grabs the keyboard: feed it Escape and swallow keys so
		 * they neither trigger bindings nor reach clients. */
		if (server->menu != nullptr) {
			for (int i = 0; i < nsyms; i++) {
				if (server->menu->on_key(syms[i])) {
					/* The key dismissed the menu. Selecting an entry yields its
					 * actions, run only AFTER the widget is destroyed (no
					 * reentrancy), mirroring the pointer path in cursor.cpp. */
					std::vector<wb::Action> actions =
							server->menu->take_actions();
					server->menu.reset();
					wb_run_menu_actions(server, std::move(actions));
					break;
				}
			}
			idle_activity(server, seat);
			return;
		}
		/* Super+Escape: shortcuts back from (or again to) a program that
		 * took them (keyboard-shortcuts-inhibit). */
		bool logo_only = (modifiers & ~WLR_MODIFIER_CAPS & ~WLR_MODIFIER_MOD2) ==
				WLR_MODIFIER_LOGO;
		for (int i = 0; i < nsyms && !handled; i++)
			if (syms[i] == XKB_KEY_Escape && logo_only)
				handled = shortcuts_inhibit_toggle(server);
		if (handled) {
			/* consumed */
		} else if (shortcuts_inhibited(server)) {
			/* The focused program has the keys; only VT switching stays. */
			if ((modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) &&
					server->session != nullptr)
				for (int i = 0; i < nsyms && !handled; i++)
					if (syms[i] >= XKB_KEY_XF86Switch_VT_1 &&
							syms[i] <= XKB_KEY_XF86Switch_VT_12) {
						wlr_session_change_vt(server->session,
								syms[i] - XKB_KEY_XF86Switch_VT_1 + 1);
						handled = true;
					}
		} else {
			/* The first binding that runs ends it: a binding can free the
			 * keymap `syms` points into (Reconfigure), and a later keysym
			 * mustn't undo `handled`. */
			std::vector<wb::Action> repeat;
			for (int i = 0; i < nsyms && !handled; i++)
				handled = handle_keybinding(server, syms[i], modifiers, &repeat);
			if (handled && !repeat.empty())
				start_repeat(std::move(repeat), modifiers);
		}
	} else if (server->menu != nullptr || server->switcher != nullptr) {
		/* Swallow key releases too while a menu or the switcher OSD is up.
		 * (The modifier-release that commits the switcher arrives via
		 * on_modifiers, not here.) */
		idle_activity(server, seat);
		return;
	}

	if (!handled && input_method_key(server, keyboard, event)) {
		/* The input method has the keyboard; what it doesn't use it passes
		 * on through its virtual keyboard. */
		idle_activity(server, seat);
		return;
	}
	if (!handled) {
		/* Pass it along to the focused client. */
		wlr_seat_set_keyboard(seat, keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}

	idle_activity(server, seat);
}

void wb_keyboard::stop_repeat() {
	if (repeat_timer != nullptr)
		wl_event_source_remove(repeat_timer);
	repeat_timer = nullptr;
	repeat_actions.clear();
}

/* The keyboard's repeat delay and rate (a virtual keyboard may have none
 * set: the usual 600 ms, 25 a second). */
static void repeat_timing(struct wlr_keyboard *kb, int *delay_ms, int *interval_ms) {
	int rate = kb->repeat_info.rate > 0 ? kb->repeat_info.rate : 25;
	*delay_ms = kb->repeat_info.delay > 0 ? kb->repeat_info.delay : 600;
	*interval_ms = std::max(1, 1000 / rate);
}

void wb_keyboard::start_repeat(std::vector<wb::Action> actions, uint32_t mods) {
	stop_repeat();
	repeat_actions = std::move(actions);
	repeat_modifiers = mods;
	repeat_timer = wl_event_loop_add_timer(server->wl_event_loop, [](void *data) {
		auto *kb = static_cast<wb_keyboard *>(data);
		struct wb_server *server = kb->server;
		/* Not while something else has the keyboard, nor locked. */
		if (session_locked(server) || server->menu != nullptr || server->switcher != nullptr ||
				server->about_card != nullptr ||
				wlr_keyboard_get_modifiers(kb->keyboard) != kb->repeat_modifiers) {
			kb->stop_repeat();
			return 0;
		}
		int delay = 0, interval = 0;
		repeat_timing(kb->keyboard, &delay, &interval);
		wl_event_source_timer_update(kb->repeat_timer, interval);
		/* A copy: an action may end the repeat (or free this keyboard's
		 * state by reconfiguring). */
		std::vector<wb::Action> actions = kb->repeat_actions;
		server->last_key_modifiers = kb->repeat_modifiers;
		wb::run_actions(actions, server);
		return 0;
	}, this);
	int delay = 0, interval = 0;
	repeat_timing(keyboard, &delay, &interval);
	wl_event_source_timer_update(repeat_timer, delay);
}

/* Give a keyboard the keymap from rc.xml's <keyboard><xkb> (or xkbcommon's
 * default, which follows the XKB_DEFAULT_* environment variables). */
static void apply_keymap(struct wb_server *server, struct wlr_keyboard *kb) {
	/* Prepare an XKB keymap and assign it to the keyboard. A NULL rule_names
	 * pointer makes libxkbcommon fall back to the XKB_* env variables; we use a
	 * zero-initialized stack struct so any field not set from the config stays
	 * NULL (xkbcommon's "default") rather than an uninitialized garbage pointer. */
	struct xkb_rule_names rule_names = {};
	struct xkb_rule_names *rules = nullptr;
	if (server->config && server->config->keyboard_layout.use_config) {
		rule_names.layout = server->config->keyboard_layout.layout;
		rule_names.model = server->config->keyboard_layout.model;
		rule_names.options = server->config->keyboard_layout.options;
		rule_names.rules = server->config->keyboard_layout.rules;
		rule_names.variant = server->config->keyboard_layout.variant;
		rules = &rule_names;
	}

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, rules,
		XKB_KEYMAP_COMPILE_NO_FLAGS);

	if (keymap != nullptr) {
		wlr_keyboard_set_keymap(kb, keymap);
		wlr_keyboard_set_repeat_info(kb, 25, 600);
	}
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	if (kb->keymap != nullptr)
		wlr_log(WLR_INFO, "wb-keymap %s", xkb_keymap_layout_get_name(kb->keymap, 0));
}

static void handle_new_keyboard(struct wb_server *server,
		struct wlr_input_device *device) {
	auto *keyboard = new wb_keyboard{};
	keyboard->server = server;
	keyboard->keyboard = wlr_keyboard_from_input_device(device);

	apply_keymap(server, keyboard->keyboard);

	keyboard->modifiers.connect(&keyboard->keyboard->events.modifiers,
			[keyboard](void *data) { keyboard->on_modifiers(data); });
	keyboard->key.connect(&keyboard->keyboard->events.key,
			[keyboard](void *data) { keyboard->on_key(data); });
	keyboard->destroy.connect(&device->events.destroy,
			[keyboard](void *data) {
		/* The wb::Listener members disconnect themselves on delete. */
		wl_list_remove(&keyboard->link);
		delete keyboard;
	});

	/* The seat's keyboard (its keys and modifiers go with focus): not a
	 * virtual one <virtualInput> blocks now. */
	if (!wb_input_blocked(server, device))
		wlr_seat_set_keyboard(server->seat->seat, keyboard->keyboard);

	wl_list_insert(&server->seat->keyboards, &keyboard->link);
}

void seat_recheck_keyboard(struct wb_server *server) {
	struct wlr_keyboard *now = wlr_seat_get_keyboard(server->seat->seat);
	if (now == nullptr || !wb_input_blocked(server, &now->base))
		return;
	/* A blocked one (a virtual keyboard, the screen locked): the first
	 * keyboard allowed, or none, so focus doesn't carry its state. */
	struct wlr_keyboard *pick = nullptr;
	struct wb_keyboard *k;
	wl_list_for_each(k, &server->seat->keyboards, link)
		if (pick == nullptr && !wb_input_blocked(server, &k->keyboard->base))
			pick = k->keyboard;
	wlr_seat_set_keyboard(server->seat->seat, pick);
	wlr_log(WLR_INFO, "wb-seat keyboard: a blocked one let go (now %s)",
			pick != nullptr ? "another" : "none");
}

#ifdef HAS_LIBINPUT
static bool libinput_config_get_enabled(char *config) {
	return strcmp(config, "disabled") != 0;
}
#endif

#ifdef HAS_LIBINPUT
/* Apply rc.xml's <libinput> settings to a pointer device. A setting that
 * isn't given goes back to the device's default, so removing one from
 * rc.xml and reconfiguring undoes it. */
static void apply_pointer_config(struct wb_server *server, struct wlr_input_device *device) {
	if (!wlr_input_device_is_libinput(device))
		return;
	struct libinput_device *d = wlr_libinput_get_device_handle(device);
	const struct wb_config *config = server->config;
	auto get = [config](char *const wb_config::libinput_config_t::*field) -> const char * {
		if (config == nullptr || !config->libinput_config.use_config)
			return nullptr;
		return config->libinput_config.*field;
	};
	auto on = [](const char *v, bool fallback) {
		return v != nullptr ? libinput_config_get_enabled(const_cast<char *>(v)) : fallback;
	};
	using L = wb_config::libinput_config_t;

	if (libinput_device_config_accel_is_available(d)) {
		const char *v = get(&L::accel_profile);
		enum libinput_config_accel_profile profile =
				libinput_device_config_accel_get_default_profile(d);
		if (v != nullptr)
			profile = strcmp(v, "flat") == 0 ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
				: strcmp(v, "none") == 0 ? LIBINPUT_CONFIG_ACCEL_PROFILE_NONE
				: LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
		libinput_device_config_accel_set_profile(d, profile);
		const char *speed = get(&L::accel_speed);
		libinput_device_config_accel_set_speed(d, speed != nullptr ? strtod(speed, nullptr)
				: libinput_device_config_accel_get_default_speed(d));
	}
	if (libinput_device_config_calibration_has_matrix(d)) {
		float matrix[6] = {};
		libinput_device_config_calibration_get_default_matrix(d, matrix);
		if (const char *v = get(&L::calibration_matrix)) {
			/* Six space-separated floats, parsed from a copy (strtok_r is
			 * destructive); anything else keeps the default. */
			float parsed[6] = {};
			unsigned short i = 0;
			std::string copy = v;
			char *saveptr = NULL;
			for (char *t = strtok_r(copy.data(), " ", &saveptr); t != NULL && i < 6;
					t = strtok_r(NULL, " ", &saveptr))
				parsed[i++] = strtof(t, NULL);
			if (i == 6)
				std::copy(parsed, parsed + 6, matrix);
		}
		libinput_device_config_calibration_set_matrix(d, matrix);
	}
	if (libinput_device_config_click_get_methods(d) != 0) {
		const char *v = get(&L::click_method);
		enum libinput_config_click_method method =
				libinput_device_config_click_get_default_method(d);
		if (v != nullptr)
			method = strcmp(v, "clickfinger") == 0 ? LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER
				: strcmp(v, "none") == 0 ? LIBINPUT_CONFIG_CLICK_METHOD_NONE
				: LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
		libinput_device_config_click_set_method(d, method);
	}
	if (libinput_device_config_dwt_is_available(d))
		libinput_device_config_dwt_set_enabled(d, on(get(&L::dwt),
				libinput_device_config_dwt_get_default_enabled(d) ==
					LIBINPUT_CONFIG_DWT_ENABLED)
				? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
	if (libinput_device_config_dwtp_is_available(d))
		libinput_device_config_dwtp_set_enabled(d, on(get(&L::dwtp),
				libinput_device_config_dwtp_get_default_enabled(d) ==
					LIBINPUT_CONFIG_DWTP_ENABLED)
				? LIBINPUT_CONFIG_DWTP_ENABLED : LIBINPUT_CONFIG_DWTP_DISABLED);
	if (libinput_device_config_left_handed_is_available(d))
		libinput_device_config_left_handed_set(d, on(get(&L::left_handed),
				libinput_device_config_left_handed_get_default(d)));
	if (libinput_device_config_middle_emulation_is_available(d))
		libinput_device_config_middle_emulation_set_enabled(d, on(get(&L::middle_emulation),
				libinput_device_config_middle_emulation_get_default_enabled(d) ==
					LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED)
				? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED
				: LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);
	if (libinput_device_config_scroll_has_natural_scroll(d))
		libinput_device_config_scroll_set_natural_scroll_enabled(d, on(get(&L::natural_scroll),
				libinput_device_config_scroll_get_default_natural_scroll_enabled(d)));
	if (libinput_device_config_scroll_get_methods(d) != 0) {
		const char *v = get(&L::scroll_method);
		enum libinput_config_scroll_method method =
				libinput_device_config_scroll_get_default_method(d);
		if (v != nullptr)
			method = strcmp(v, "edge") == 0 ? LIBINPUT_CONFIG_SCROLL_EDGE
				: strcmp(v, "none") == 0 ? LIBINPUT_CONFIG_SCROLL_NO_SCROLL
				: strcmp(v, "button") == 0 ? LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN
				: LIBINPUT_CONFIG_SCROLL_2FG;
		libinput_device_config_scroll_set_method(d, method);
		uint32_t button = libinput_device_config_scroll_get_default_button(d);
		if (const char *b = get(&L::scroll_button)) {
			int code = libevdev_event_code_from_name(EV_KEY, b);
			if (code != -1)
				button = static_cast<uint32_t>(code);
		}
		libinput_device_config_scroll_set_button(d, button);
		libinput_device_config_scroll_set_button_lock(d,
				on(get(&L::scroll_button_lock),
					libinput_device_config_scroll_get_default_button_lock(d) ==
						LIBINPUT_CONFIG_SCROLL_BUTTON_LOCK_ENABLED)
				? LIBINPUT_CONFIG_SCROLL_BUTTON_LOCK_ENABLED
				: LIBINPUT_CONFIG_SCROLL_BUTTON_LOCK_DISABLED);
	}
	if (libinput_device_config_tap_get_finger_count(d) > 0) {
		libinput_device_config_tap_set_enabled(d, on(get(&L::tap),
				libinput_device_config_tap_get_default_enabled(d) ==
					LIBINPUT_CONFIG_TAP_ENABLED)
				? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
		const char *map = get(&L::tap_button_map);
		libinput_device_config_tap_set_button_map(d, map != nullptr
				? (strcmp(map, "lmr") == 0 ? LIBINPUT_CONFIG_TAP_MAP_LMR
					: LIBINPUT_CONFIG_TAP_MAP_LRM)
				: libinput_device_config_tap_get_default_button_map(d));
		libinput_device_config_tap_set_drag_enabled(d, on(get(&L::tap_drag),
				libinput_device_config_tap_get_default_drag_enabled(d) ==
					LIBINPUT_CONFIG_DRAG_ENABLED)
				? LIBINPUT_CONFIG_DRAG_ENABLED : LIBINPUT_CONFIG_DRAG_DISABLED);
		/* "enabled" is the timed drag lock (LIBINPUT_CONFIG_DRAG_LOCK_ENABLED,
		 * every libinput version); unset keeps the device's default, whatever
		 * mode that is. */
		if (const char *lock = get(&L::tap_drag_lock))
			libinput_device_config_tap_set_drag_lock_enabled(d,
					on(lock, false) ? LIBINPUT_CONFIG_DRAG_LOCK_ENABLED
						: LIBINPUT_CONFIG_DRAG_LOCK_DISABLED);
		else
			libinput_device_config_tap_set_drag_lock_enabled(d,
					libinput_device_config_tap_get_default_drag_lock_enabled(d));
	}
}
#endif

static void handle_new_pointer(struct wb_server *server, struct wlr_input_device *device) {
#ifdef HAS_LIBINPUT
	apply_pointer_config(server, device);
#endif
	/* Remembered, so Reconfigure can apply changed settings to it. */
	auto *pointer = new wb_pointer{};
	pointer->device = device;
	pointer->destroy.connect(&device->events.destroy, [server, pointer](void *) {
		auto &list = server->seat->pointers;
		list.erase(std::remove(list.begin(), list.end(), pointer), list.end());
		delete pointer;
	});
	server->seat->pointers.push_back(pointer);

	wlr_cursor_attach_input_device(server->cursor->cursor, device);
}

static void update_seat_capabilities(struct wb_server *server);

static void handle_new_touch(struct wb_server *server, struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(server->cursor->cursor, device);
	/* A touchscreen built into a monitor says which: touches map onto it,
	 * not onto the whole layout. */
	struct wlr_touch *touch = wlr_touch_from_input_device(device);
	if (touch->output_name != nullptr) {
		struct wb_output *o;
		wl_list_for_each(o, &server->outputs, link)
			if (strcmp(o->wlr_output->name, touch->output_name) == 0)
				wlr_cursor_map_input_to_output(server->cursor->cursor, device, o->wlr_output);
	}
	++server->seat->touch_devices;
	auto *gone = new wb::Listener;
	gone->connect(&device->events.destroy, [server, gone](void *) {
		/* Its touches can't end now: end them all. */
		cursor_touch_cancel_all(server);
		--server->seat->touch_devices;
		update_seat_capabilities(server);
		gone->disconnect();
		delete gone;
	});
}

/* Advertise the seat's input capabilities to clients. waybox always owns a
 * cursor, so POINTER is always advertised; KEYBOARD is advertised once at least
 * one keyboard exists. This must run whenever an input device is added through
 * ANY path — including the virtual-pointer/virtual-keyboard protocols, which
 * bypass new_input_notify — otherwise clients never receive input from a
 * virtual device (e.g. under the headless backend used by the tests). */
static void update_seat_capabilities(struct wb_server *server) {
	/* Always advertise a keyboard, as with the pointer: a client only creates
	 * its wl_keyboard when the capability is present, so withholding it until
	 * a keyboard appears means a window focused before then (headless, or a
	 * keyboard plugged in later) misses wl_keyboard.enter and the first keys
	 * typed race the client binding its wl_keyboard. */
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
	if (server->seat->touch_devices > 0)
		caps |= WL_SEAT_CAPABILITY_TOUCH;
	wlr_seat_set_capabilities(server->seat->seat, caps);
}

static void new_input_notify(struct wb_server *server, void *data) {
	struct wlr_input_device *device = static_cast<struct wlr_input_device *>(data);
	switch (device->type) {
		case WLR_INPUT_DEVICE_KEYBOARD:
			wlr_log(WLR_INFO, "%s: %s", _("New keyboard detected"), device->name);
			handle_new_keyboard(server, device);
			break;
		case WLR_INPUT_DEVICE_POINTER:
			wlr_log(WLR_INFO, "%s: %s", _("New pointer detected"), device->name);
			handle_new_pointer(server, device);
			break;
		case WLR_INPUT_DEVICE_TOUCH:
			wlr_log(WLR_INFO, "%s: %s", _("New touchscreen detected"), device->name);
			handle_new_touch(server, device);
			break;
		default:
			wlr_log(WLR_INFO, "%s: %s", _("Unsupported input device detected"), device->name);
			break;
	}

	update_seat_capabilities(server);
}

void seat_focus_surface(struct wb_seat *seat, struct wlr_surface *surface) {
	if (!surface) {
		wlr_seat_keyboard_notify_clear_focus(seat->seat);
		return;
	}
	/* While the session is locked, only the locker may hold keyboard focus.
	 * This is the single place keyboard enter is sent, so it covers window
	 * focus, layer-shell focus and activation requests alike. */
	if (session_locked(seat->server) &&
			!session_lock_owns_surface(seat->server, surface))
		return;

	/* Always send enter, even with no keyboard attached yet (headless, or a
	 * keyboard hot-plugged later): otherwise the surface never receives
	 * wl_keyboard.enter and ignores keys once a keyboard does appear. */
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat->seat);
	if (kb != NULL) {
		wlr_seat_keyboard_notify_enter(seat->seat, surface, kb->keycodes,
			kb->num_keycodes, &kb->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(seat->seat, surface, NULL, 0, NULL);
	}
}

void seat_set_focus_layer(struct wb_seat *seat, struct wlr_layer_surface_v1 *layer) {
	if (!layer) {
		seat->focused_layer = NULL;
		return;
	}
	seat_focus_surface(seat, layer->surface);
	if (layer->current.layer > ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
		seat->focused_layer = layer;
	}
}

namespace {
const struct wlr_touch_impl test_touch_impl = {.name = "waybox-test-touch"};
}

void seat_init_test_touch(struct wb_server *server) {
	const char *on = getenv("WAYBOX_TEST_TOUCH");
	if (on == nullptr || strcmp(on, "1") != 0)
		return;
	auto *t = new wlr_touch{};
	wlr_touch_init(t, &test_touch_impl, "waybox-test-touch");
	server->seat->test_touch = t;
	wlr_log(WLR_INFO, "%s", "Test touchscreen (WAYBOX_TEST_TOUCH=1)");
	handle_new_touch(server, &t->base);
	update_seat_capabilities(server);
}

void seat_finish_test_touch(struct wb_server *server) {
	if (server->seat == nullptr || server->seat->test_touch == nullptr)
		return;
	struct wlr_touch *t = server->seat->test_touch;
	server->seat->test_touch = nullptr;
	wlr_touch_finish(t);  /* its destroy: off the cursor, the count down */
	delete t;
}

bool seat_test_touch(struct wb_server *server, const std::string &what, int32_t id,
		double x, double y) {
	struct wlr_touch *t = server->seat->test_touch;
	if (t == nullptr)
		return false;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	uint32_t ms = static_cast<uint32_t>(now.tv_sec * 1000 + now.tv_nsec / 1000000);
	if (what == "down") {
		struct wlr_touch_down_event e = {t, ms, id, x, y};
		wl_signal_emit_mutable(&t->events.down, &e);
	} else if (what == "motion") {
		struct wlr_touch_motion_event e = {t, ms, id, x, y};
		wl_signal_emit_mutable(&t->events.motion, &e);
	} else if (what == "up") {
		struct wlr_touch_up_event e = {t, ms, id};
		wl_signal_emit_mutable(&t->events.up, &e);
	} else if (what == "frame") {
		wl_signal_emit_mutable(&t->events.frame, t);
	} else {
		return false;
	}
	return true;
}

struct wb_seat *wb_seat_create(struct wb_server *server) {
	auto *seat = new wb_seat{};
	seat->server = server;
	wl_list_init(&seat->keyboards);
	server->new_input.connect(&server->backend->events.new_input,
			[server](void *data) { new_input_notify(server, data); });

	/* Virtual input. These let clients (notably test harnesses and tools like
	 * wtype/ydotool/wlrctl) inject keyboard and pointer events; virtual devices
	 * are routed through the same handlers as physical ones. Note: this allows
	 * any client to synthesise input, which is a security trade-off a future
	 * config knob may want to gate. */
	server->virtual_keyboard_manager =
		wlr_virtual_keyboard_manager_v1_create(server->wl_display);
	server->new_virtual_keyboard.connect(
			&server->virtual_keyboard_manager->events.new_virtual_keyboard,
			[server](void *data) {
		auto *virtual_keyboard =
			static_cast<struct wlr_virtual_keyboard_v1 *>(data);
		virtual_keyboard->keyboard.base.data = &virtual_device_tag;
		handle_new_keyboard(server, &virtual_keyboard->keyboard.base);
		update_seat_capabilities(server);
	});
	server->virtual_pointer_manager =
		wlr_virtual_pointer_manager_v1_create(server->wl_display);
	server->new_virtual_pointer.connect(
			&server->virtual_pointer_manager->events.new_virtual_pointer,
			[server](void *data) {
		auto *event =
			static_cast<struct wlr_virtual_pointer_v1_new_pointer_event *>(data);
		event->new_pointer->pointer.base.data = &virtual_device_tag;
		handle_new_pointer(server, &event->new_pointer->pointer.base);
		update_seat_capabilities(server);
	});

	seat->seat = wlr_seat_create(server->wl_display, "seat0");
	/* Advertise pointer + keyboard from the start (see
	 * update_seat_capabilities), not only once the first device appears. */
	wlr_seat_set_capabilities(seat->seat,
			WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

	wlr_primary_selection_v1_device_manager_create(server->wl_display);
	seat->request_set_primary_selection.connect(
			&seat->seat->events.request_set_primary_selection,
			[seat](void *data) {
		auto *event =
			static_cast<struct wlr_seat_request_set_primary_selection_event *>(data);
		/* Locked: the selections stay as they were (nothing set at the
		 * lock screen, or by a clipboard manager meanwhile, outlives it). */
		if (session_locked(seat->server)) {
			if (event->source != nullptr)
				wlr_primary_selection_source_destroy(event->source);
			return;
		}
		wlr_seat_set_primary_selection(seat->seat, event->source, event->serial);
	});
	seat->request_set_selection.connect(
			&seat->seat->events.request_set_selection, [seat](void *data) {
		auto *event =
			static_cast<struct wlr_seat_request_set_selection_event *>(data);
		/* Locked: as the primary selection above (the one copied before
		 * the lock is still there after it). */
		if (session_locked(seat->server)) {
			if (event->source != nullptr)
				wlr_data_source_destroy(event->source);
			return;
		}
		wlr_seat_set_selection(seat->seat, event->source, event->serial);
	});

	/* Drag-and-drop: wlroots only asks; without this no drag ever started
	 * (files from a file manager, text, browser tabs). A drag needs the
	 * button press it comes from, as with every grab. */
	seat->request_start_drag.connect(&seat->seat->events.request_start_drag,
			[seat](void *data) {
		auto *event = static_cast<struct wlr_seat_request_start_drag_event *>(data);
		/* None while the screen is locked (a drag takes the keyboard grab,
		 * the lock screen's password field with it). */
		if (session_locked(seat->server)) {
			wlr_log(WLR_DEBUG, "%s", "Refused a drag while the screen is locked");
			if (event->drag->source != nullptr)
				wlr_data_source_destroy(event->drag->source);
			return;
		}
		if (wlr_seat_validate_pointer_grab_serial(seat->seat, event->origin,
				event->serial)) {
			wlr_seat_start_pointer_drag(seat->seat, event->drag, event->serial);
			return;
		}
		struct wlr_touch_point *point = nullptr;
		if (wlr_seat_validate_touch_grab_serial(seat->seat, event->origin,
				event->serial, &point)) {
			wlr_seat_start_touch_drag(seat->seat, event->drag, event->serial, point);
			return;
		}
		wlr_log(WLR_DEBUG, "%s", "Refused a drag without a button held");
		if (event->drag->source != nullptr)
			wlr_data_source_destroy(event->drag->source);
	});
	seat->start_drag.connect(&seat->seat->events.start_drag, [seat](void *data) {
		auto *drag = static_cast<struct wlr_drag *>(data);
		struct wb_server *server = seat->server;
		wlr_log(WLR_DEBUG, "wb-drag start icon=%d", drag->icon != nullptr);
		if (drag->icon != nullptr) {
			/* Follows the icon surface; destroyed with it. */
			wlr_scene_drag_icon_create(server->drag_icon_tree, drag->icon);
			wlr_scene_node_set_position(&server->drag_icon_tree->node,
					static_cast<int>(server->cursor->cursor->x),
					static_cast<int>(server->cursor->cursor->y));
			wlr_scene_node_raise_to_top(&server->drag_icon_tree->node);
		}
		seat->drag_destroy.connect(&drag->events.destroy, [seat](void *) {
			seat->drag_destroy.disconnect();
			/* The pointer goes back to whatever is under it now. */
			cursor_refresh_focus(seat->server);
		});
	});

	return seat;
}

wb_seat::~wb_seat() {
	/* Free any keyboards still attached (their own destroy handlers normally
	 * remove them, but be defensive at shutdown). The wb::Listener members of
	 * both the keyboards and the seat disconnect themselves on destruction. */
	struct wb_keyboard *keyboard, *tmp;
	wl_list_for_each_safe(keyboard, tmp, &keyboards, link) {
		wl_list_remove(&keyboard->link);
		delete keyboard;
	}
	for (struct wb_pointer *pointer : pointers)
		delete pointer;
	pointers.clear();
}

void seat_reconfigure_input(struct wb_server *server) {
	/* Physical keyboards get the (possibly changed) keymap; virtual ones
	 * (wtype, input methods) keep the keymap their client gave them. */
	struct wb_keyboard *keyboard;
	wl_list_for_each(keyboard, &server->seat->keyboards, link)
		if (!wb_input_is_virtual(&keyboard->keyboard->base))
			apply_keymap(server, keyboard->keyboard);
#ifdef HAS_LIBINPUT
	for (struct wb_pointer *pointer : server->seat->pointers)
		apply_pointer_config(server, pointer->device);
#endif
}
