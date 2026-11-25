/*
 * Session restore: xdg-session-management-v1 (wlroots has none, so it's
 * Waybox's own). An app gets a session (a new id, or one it kept from a
 * previous run) and names its windows in it; Waybox keeps their state in
 * $XDG_STATE_HOME/waybox/sessions.json and, when the app asks before a
 * window's first commit, opens it where it was. See xdg_session.h.
 */
#include "waybox/xdg_session.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "xdg-session-management-v1-protocol.h"

#include "config.h"
#include "waybox/server.h"
#include "waybox/session_store.hpp"
#include "waybox/xdg_shell.h"

namespace fs = std::filesystem;

struct wb_session;

/* An xdg_toplevel_session_v1: the app's handle for one window. Inert once
 * its session or window is gone (requests then do nothing). */
struct wb_toplevel_session {
	struct wl_resource *resource = nullptr;
	struct wb_session *session = nullptr;
	struct wb_toplevel *toplevel = nullptr;
	std::string name;
};

/* An xdg_session_v1. Inert once destroyed by the app or replaced. */
struct wb_session {
	struct wl_resource *resource = nullptr;
	struct wb_session_state *state = nullptr;
	std::string id;
	uint32_t reason = 0;
	bool inert = false;
	std::vector<struct wb_toplevel_session *> toplevels;
};

struct wb_session_state {
	struct wb_server *server = nullptr;
	struct wl_global *global = nullptr;
	wb::SessionStore store;
	std::string path;  /* sessions.json */
	std::vector<struct wb_session *> sessions;  /* live, not inert */
	struct wl_event_source *save_timer = nullptr;
	bool dirty = false;
	bool armed = false;  /* the save timer is set */
};

namespace {

constexpr int kSaveDelayMs = 1000;

int64_t now_seconds() {
	return static_cast<int64_t>(time(nullptr));
}

bool enabled(struct wb_session_state *st) {
	return st->server->config == nullptr || st->server->config->session_restore;
}

std::string state_path() {
	const char *state = getenv("XDG_STATE_HOME");
	std::string dir;
	if (state != nullptr && state[0] == '/')
		dir = state;
	else if (const char *home = getenv("HOME"); home != nullptr && home[0] == '/')
		dir = std::string(home) + "/.local/state";
	else
		return "";
	return dir + "/waybox/sessions.json";
}

void save_now(struct wb_session_state *st) {
	st->dirty = false;
	if (st->path.empty())
		return;
	st->store.expire(now_seconds());
	std::error_code ec;
	fs::create_directories(fs::path(st->path).parent_path(), ec);
	std::string tmp = st->path + ".tmp";
	int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) {
		wlr_log(WLR_ERROR, "sessions: can't write %s: %s", tmp.c_str(), strerror(errno));
		return;
	}
	std::string text = st->store.to_json();
	bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
	ok = fsync(fd) == 0 && ok;
	close(fd);
	if (!ok || rename(tmp.c_str(), st->path.c_str()) != 0) {
		wlr_log(WLR_ERROR, "sessions: can't save %s", st->path.c_str());
		unlink(tmp.c_str());
	}
}

int on_save_timer(void *data) {
	auto *st = static_cast<struct wb_session_state *>(data);
	st->armed = false;
	/* The windows as they are now: one snapshot for many changes. */
	for (struct wb_session *s : st->sessions)
		for (struct wb_toplevel_session *h : s->toplevels)
			if (h->toplevel != nullptr)
				xdg_session_snapshot(h->toplevel);
	save_now(st);
	return 0;
}

/* Saved kSaveDelayMs after the first change, not pushed back by more:
 * a window that changes all the time (a video's every commit) still gets
 * saved. */
void schedule_save(struct wb_session_state *st) {
	st->dirty = true;
	if (st->save_timer != nullptr && !st->armed) {
		wl_event_source_timer_update(st->save_timer, kSaveDelayMs);
		st->armed = true;
	}
}

/* The handle no longer stands for a window (its session or window went,
 * or the app let it go). */
void detach_handle(struct wb_toplevel_session *h) {
	if (h->toplevel != nullptr && h->toplevel->session_handle == h)
		h->toplevel->session_handle = nullptr;
	h->toplevel = nullptr;
	if (h->session != nullptr) {
		auto &list = h->session->toplevels;
		list.erase(std::remove(list.begin(), list.end(), h), list.end());
		h->session = nullptr;
	}
}

void make_inert(struct wb_session *s) {
	if (s->inert)
		return;
	s->inert = true;
	for (struct wb_toplevel_session *h : std::vector<struct wb_toplevel_session *>(s->toplevels))
		detach_handle(h);
	auto &list = s->state->sessions;
	list.erase(std::remove(list.begin(), list.end(), s), list.end());
}

/* ---- xdg_toplevel_session_v1 ---- */

void handle_destroy(struct wl_client *, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

void handle_rename(struct wl_client *, struct wl_resource *resource, const char *name) {
	auto *h = static_cast<struct wb_toplevel_session *>(wl_resource_get_user_data(resource));
	if (h == nullptr || h->session == nullptr)
		return;
	struct wb_session *s = h->session;
	if (!wb::valid_utf8(name)) {
		wl_resource_post_error(s->resource, XDG_SESSION_V1_ERROR_INVALID_NAME,
				"toplevel name isn't valid UTF-8");
		return;
	}
	for (struct wb_toplevel_session *o : s->toplevels)
		if (o != h && o->name == name) {
			wl_resource_post_error(s->resource, XDG_SESSION_V1_ERROR_NAME_IN_USE,
					"toplevel name already in use");
			return;
		}
	auto &windows = s->state->store.sessions[s->id].windows;
	if (auto it = windows.find(h->name); it != windows.end()) {
		wb::SavedWindow w = it->second;
		windows.erase(it);
		s->state->store.set_window(s->id, name, w);
	}
	h->name = name;
	schedule_save(s->state);
}

const struct xdg_toplevel_session_v1_interface toplevel_session_impl = {
	.destroy = handle_destroy,
	.rename = handle_rename,
};

void handle_resource_destroyed(struct wl_resource *resource) {
	auto *h = static_cast<struct wb_toplevel_session *>(wl_resource_get_user_data(resource));
	if (h == nullptr)
		return;
	detach_handle(h);
	delete h;
}

/* ---- xdg_session_v1 ---- */

struct wb_toplevel *toplevel_of(struct wb_server *server, struct wl_resource *xdg_toplevel) {
	struct wlr_xdg_toplevel *xt = wlr_xdg_toplevel_from_resource(xdg_toplevel);
	if (xt == nullptr)
		return nullptr;
	struct wb_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link)
		if (t->kind == wb_view_kind::Xdg && t->xdg_toplevel == xt)
			return t;
	return nullptr;
}

/* add_toplevel and restore_toplevel: the checks, and the handle. */
struct wb_toplevel_session *new_handle(struct wl_client *client, struct wl_resource *session_res,
		uint32_t id, struct wl_resource *xdg_toplevel, const char *name, bool restoring) {
	auto *s = static_cast<struct wb_session *>(wl_resource_get_user_data(session_res));
	struct wl_resource *resource = wl_resource_create(client,
			&xdg_toplevel_session_v1_interface, wl_resource_get_version(session_res), id);
	if (resource == nullptr) {
		wl_client_post_no_memory(client);
		return nullptr;
	}
	auto *h = new wb_toplevel_session;
	h->resource = resource;
	h->name = name;
	wl_resource_set_implementation(resource, &toplevel_session_impl, h,
			handle_resource_destroyed);
	if (s == nullptr || s->inert)
		return h;  /* an inert handle */
	if (!wb::valid_utf8(name)) {
		wl_resource_post_error(session_res, XDG_SESSION_V1_ERROR_INVALID_NAME,
				"toplevel name isn't valid UTF-8");
		return nullptr;
	}
	for (struct wb_toplevel_session *o : s->toplevels)
		if (o->name == name) {
			wl_resource_post_error(session_res, XDG_SESSION_V1_ERROR_NAME_IN_USE,
					"toplevel name already in use");
			return nullptr;
		}
	struct wb_toplevel *t = toplevel_of(s->state->server, xdg_toplevel);
	if (t == nullptr)
		return h;  /* its window is gone already: inert */
	if (t->session_handle != nullptr) {
		wl_resource_post_error(session_res, XDG_SESSION_V1_ERROR_ALREADY_ADDED,
				"toplevel already added to a session");
		return nullptr;
	}
	if (restoring && t->xdg_toplevel->base->initialized) {
		wl_resource_post_error(session_res, XDG_SESSION_V1_ERROR_ALREADY_MAPPED,
				"restore_toplevel after the toplevel's first commit");
		return nullptr;
	}
	h->session = s;
	h->toplevel = t;
	s->toplevels.push_back(h);
	t->session_handle = h;
	return h;
}

void session_add_toplevel(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *xdg_toplevel, const char *name) {
	struct wb_toplevel_session *h = new_handle(client, resource, id, xdg_toplevel, name, false);
	if (h != nullptr && h->toplevel != nullptr) {
		/* Its state from now on, not what was kept under the name. */
		h->session->state->store.sessions[h->session->id].windows.erase(h->name);
		if (h->toplevel->mapped)
			xdg_session_toplevel_changed(h->toplevel);
	}
}

void session_restore_toplevel(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *xdg_toplevel, const char *name) {
	struct wb_toplevel_session *h = new_handle(client, resource, id, xdg_toplevel, name, true);
	if (h == nullptr || h->toplevel == nullptr)
		return;
	struct wb_session *s = h->session;
	if (!enabled(s->state))
		return;
	auto &windows = s->state->store.sessions[s->id].windows;
	auto it = windows.find(h->name);
	if (it == windows.end())
		return;  /* not known: as add_toplevel */
	h->toplevel->session_restore = it->second;
	h->toplevel->session_restore_desktop = s->reason != XDG_SESSION_MANAGER_V1_REASON_LAUNCH;
}

void session_remove_toplevel(struct wl_client *, struct wl_resource *resource, const char *name) {
	auto *s = static_cast<struct wb_session *>(wl_resource_get_user_data(resource));
	if (s == nullptr || s->inert)
		return;
	s->state->store.sessions[s->id].windows.erase(name);
	for (struct wb_toplevel_session *h : std::vector<struct wb_toplevel_session *>(s->toplevels))
		if (h->name == name)
			detach_handle(h);
	schedule_save(s->state);
}

void session_destroy(struct wl_client *, struct wl_resource *resource) {
	/* State kept; updates stop (the resource's destructor). */
	wl_resource_destroy(resource);
}

void session_remove(struct wl_client *, struct wl_resource *resource) {
	auto *s = static_cast<struct wb_session *>(wl_resource_get_user_data(resource));
	if (s != nullptr && !s->inert) {
		s->state->store.sessions.erase(s->id);
		make_inert(s);
		schedule_save(s->state);
	}
	wl_resource_destroy(resource);
}

const struct xdg_session_v1_interface session_impl = {
	.destroy = session_destroy,
	.remove = session_remove,
	.add_toplevel = session_add_toplevel,
	.restore_toplevel = session_restore_toplevel,
	.remove_toplevel = session_remove_toplevel,
};

void session_resource_destroyed(struct wl_resource *resource) {
	auto *s = static_cast<struct wb_session *>(wl_resource_get_user_data(resource));
	if (s == nullptr)
		return;
	/* Its windows' latest state, kept before updates stop. */
	if (!s->inert) {
		for (struct wb_toplevel_session *h : s->toplevels)
			if (h->toplevel != nullptr)
				xdg_session_snapshot(h->toplevel);
		schedule_save(s->state);
	}
	make_inert(s);
	delete s;
}

/* ---- xdg_session_manager_v1 ---- */

void manager_destroy(struct wl_client *, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

void manager_get_session(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		uint32_t reason, const char *session_id) {
	auto *st = static_cast<struct wb_session_state *>(wl_resource_get_user_data(resource));
	if (reason < XDG_SESSION_MANAGER_V1_REASON_LAUNCH ||
			reason > XDG_SESSION_MANAGER_V1_REASON_SESSION_RESTORE) {
		wl_resource_post_error(resource, XDG_SESSION_MANAGER_V1_ERROR_INVALID_REASON,
				"invalid reason %u", reason);
		return;
	}
	if (session_id != nullptr && !wb::valid_utf8(session_id)) {
		wl_resource_post_error(resource, XDG_SESSION_MANAGER_V1_ERROR_INVALID_SESSION_ID,
				"session id isn't valid UTF-8");
		return;
	}
	if (session_id != nullptr)
		for (struct wb_session *other : std::vector<struct wb_session *>(st->sessions)) {
			if (other->id != session_id)
				continue;
			if (wl_resource_get_client(other->resource) == client) {
				wl_resource_post_error(resource, XDG_SESSION_MANAGER_V1_ERROR_IN_USE,
						"session already in use");
				return;
			}
			/* Taken over by another client. */
			xdg_session_v1_send_replaced(other->resource);
			make_inert(other);
		}
	struct wl_resource *res = wl_resource_create(client, &xdg_session_v1_interface,
			wl_resource_get_version(resource), id);
	if (res == nullptr) {
		wl_client_post_no_memory(client);
		return;
	}
	auto *s = new wb_session;
	s->resource = res;
	s->state = st;
	s->reason = reason;
	wl_resource_set_implementation(res, &session_impl, s, session_resource_destroyed);
	bool known = enabled(st) && session_id != nullptr &&
			st->store.sessions.count(session_id) != 0;
	s->id = known ? session_id : wb::new_session_id();
	st->sessions.push_back(s);
	if (!enabled(st)) {
		/* Session restore off: a session that keeps nothing. */
		make_inert(s);
		xdg_session_v1_send_created(res, s->id.c_str());
		return;
	}
	st->store.sessions[s->id].used = now_seconds();
	if (known)
		xdg_session_v1_send_restored(res);
	else
		xdg_session_v1_send_created(res, s->id.c_str());
	wlr_log(WLR_INFO, "wb-session %s %s", known ? "restored" : "created", s->id.c_str());
	schedule_save(st);
}

const struct xdg_session_manager_v1_interface manager_impl = {
	.destroy = manager_destroy,
	.get_session = manager_get_session,
};

void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
			&xdg_session_manager_v1_interface, version, id);
	if (resource == nullptr) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, nullptr);
}

}  // namespace

void init_xdg_session(struct wb_server *server) {
	auto *st = new wb_session_state;
	st->server = server;
	st->path = state_path();
	if (!st->path.empty()) {
		std::ifstream f(st->path);
		if (f) {
			std::stringstream text;
			text << f.rdbuf();
			if (!st->store.from_json(text.str()))
				wlr_log(WLR_ERROR, "sessions: %s isn't readable; starting afresh",
						st->path.c_str());
			st->store.expire(now_seconds());
		}
	}
	st->save_timer = wl_event_loop_add_timer(server->wl_event_loop, on_save_timer, st);
	/* Under the security context, sandboxed apps get it too: it only
	 * places their own windows. */
	st->global = wl_global_create(server->wl_display, &xdg_session_manager_v1_interface, 1,
			st, bind_manager);
	server->sessions = st;
}

void finish_xdg_session(struct wb_server *server) {
	struct wb_session_state *st = server->sessions;
	if (st == nullptr)
		return;
	/* The windows as they are, then out to disk. */
	if (st->save_timer != nullptr) {
		on_save_timer(st);
		wl_event_source_remove(st->save_timer);
		st->save_timer = nullptr;
	} else if (st->dirty) {
		save_now(st);
	}
	for (struct wb_session *s : std::vector<struct wb_session *>(st->sessions))
		make_inert(s);
	if (st->global != nullptr)
		wl_global_destroy(st->global);
	server->sessions = nullptr;
	/* The global and resources go with the display; their state can't
	 * outlive this, so nothing may point here any more. */
	delete st;
}

void xdg_session_snapshot(struct wb_toplevel *t) {
	struct wb_toplevel_session *h = t->session_handle;
	if (h == nullptr || h->session == nullptr || h->session->inert || !t->mapped)
		return;
	struct wb_session_state *st = h->session->state;
	if (!enabled(st))
		return;
	struct wlr_box g = t->geometry;
	if (t->fullscreen && t->restore_fullscreen.width > 0) {
		g = t->restore_fullscreen;
	} else if (t->max_horz && t->max_vert && t->restore_maximize.width > 0) {
		g = t->restore_maximize;
	} else {
		/* The size the client settled on, which may differ from the one
		 * asked for. */
		struct wlr_box drawn = view_geometry(t);
		if (drawn.width > 0 && drawn.height > 0) {
			g.width = drawn.width;
			g.height = drawn.height;
		}
	}
	if (g.width <= 0 || g.height <= 0)
		return;
	struct wlr_output *output = get_active_output(t);
	struct wlr_box area = output_usable_area(t->server, output);
	wb::SavedWindow w;
	w.x = g.x - area.x;
	w.y = g.y - area.y;
	w.width = g.width;
	w.height = g.height;
	w.output = output != nullptr ? output->name : "";
	w.maximized = t->max_horz && t->max_vert;
	w.fullscreen = t->fullscreen;
	w.desktop = t->desktop;
	w.omnipresent = t->omnipresent;
	st->store.set_window(h->session->id, h->name, w);
	st->store.sessions[h->session->id].used = now_seconds();
}

void xdg_session_toplevel_changed(struct wb_toplevel *t) {
	struct wb_toplevel_session *h = t->session_handle;
	if (h != nullptr && h->session != nullptr && !h->session->inert && t->mapped)
		schedule_save(h->session->state);
}

void xdg_session_toplevel_unmapped(struct wb_toplevel *t) {
	if (t->session_handle != nullptr) {
		xdg_session_snapshot(t);
		if (t->session_handle->session != nullptr)
			schedule_save(t->session_handle->session->state);
	}
}

void xdg_session_toplevel_destroyed(struct wb_toplevel *t) {
	if (t->session_handle != nullptr)
		detach_handle(t->session_handle);
}

void xdg_session_initial_configure(struct wb_toplevel *t) {
	if (!t->session_restore || t->session_handle == nullptr)
		return;
	/* Before the first configure, which then has the size it had. */
	xdg_toplevel_session_v1_send_restored(t->session_handle->resource);
	wlr_log(WLR_INFO, "wb-session restore %s %dx%d", t->session_handle->name.c_str(),
			t->session_restore->width, t->session_restore->height);
}

std::vector<wb::OutputBox> xdg_session_outputs(struct wb_server *server) {
	std::vector<wb::OutputBox> out;
	struct wb_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output == nullptr || !o->wlr_output->enabled)
			continue;
		struct wlr_box a = output_usable_area(server, o->wlr_output);
		if (a.width > 0 && a.height > 0)
			out.push_back({o->wlr_output->name, {a.x, a.y, a.width, a.height}});
	}
	return out;
}
