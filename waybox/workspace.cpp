/*
 * ext-workspace-v1 server (see workspace.h). wlroots 0.19 has no helper for
 * this protocol, so the resources are managed here.
 *
 * Lifetimes: a WsClient lives as long as its manager resource, its
 * WsGroups as long as it or until no longer wanted. Group and
 * workspace resources may outlive them (a client destroys them after
 * "finished", or in any order on disconnect), so they hold a back-pointer
 * that is cleared when the WsClient goes away, and their request handlers
 * ignore a cleared one.
 */
#include "waybox/workspace.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ext-workspace-v1-protocol.h"
#include "waybox/desktops.hpp"
#include "waybox/listener.hpp"
#include "waybox/output.h"
#include "waybox/server.h"
#include "waybox/xdg_shell.h"

namespace {

struct WsClient;
struct WsGroup;

/* One ext_workspace_handle_v1 resource. */
struct WsHandle {
	WsGroup *group = nullptr;  /* null once removed or the client is gone */
	struct wl_resource *resource = nullptr;
	int index = 0;
	std::string name;
	uint32_t state = 0;
	bool sent_state = false;
};

/* A wl_output resource of the client that a group has entered. */
struct WsOutput {
	WsGroup *group = nullptr;
	struct wl_resource *resource = nullptr;
	struct wl_listener destroy = {};
};

/* One ext_workspace_group_handle_v1: shared desktops, one spanning every
 * output (`output` empty); desktops per monitor, one per monitor (its
 * name), each with its own desktops shown. */
struct WsGroup {
	WsClient *client = nullptr;
	std::string output;
	struct wl_resource *resource = nullptr;  /* null once the client destroyed it */
	std::vector<WsHandle *> workspaces;      /* index = desktop */
	std::vector<WsOutput *> outputs;
};

struct WsClient {
	struct wb_workspace_state *state = nullptr;
	struct wl_resource *manager = nullptr;
	std::vector<WsGroup *> groups;
	/* Activations, applied on commit: the group's output ("" all), desktop. */
	std::vector<std::pair<std::string, int>> pending_activate;
};

/* Watches one wlr_output for wl_output binds and destruction. */
struct OutputWatch {
	wb::Listener bind;
	wb::Listener destroy;
	bool dead = false;
};

}  // namespace

struct wb_workspace_state {
	struct wb_server *server = nullptr;
	struct wl_global *global = nullptr;
	std::vector<WsClient *> clients;
	std::map<struct wlr_output *, std::unique_ptr<OutputWatch>> watches;
	wb::Listener new_output;
	struct wl_event_source *idle = nullptr;
};

namespace {

void output_ref_destroyed(struct wl_listener *listener, void *) {
	WsOutput *o = wl_container_of(listener, o, destroy);
	if (o->group != nullptr) {
		auto &v = o->group->outputs;
		v.erase(std::remove(v.begin(), v.end(), o), v.end());
	}
	wl_list_remove(&o->destroy.link);
	delete o;
}

/* ---- ext_workspace_handle_v1 ---- */

void handle_resource_destroy(struct wl_resource *resource) {
	auto *h = static_cast<WsHandle *>(wl_resource_get_user_data(resource));
	if (h == nullptr)
		return;
	if (h->group != nullptr) {
		auto &v = h->group->workspaces;
		std::replace(v.begin(), v.end(), h, static_cast<WsHandle *>(nullptr));
	}
	delete h;
}

void handle_destroy(struct wl_client *, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

void handle_activate(struct wl_client *, struct wl_resource *resource) {
	auto *h = static_cast<WsHandle *>(wl_resource_get_user_data(resource));
	if (h != nullptr && h->group != nullptr && h->group->client != nullptr)
		h->group->client->pending_activate.emplace_back(h->group->output, h->index);
}

/* Not offered (no capability bit), so a well-behaved client never sends
 * these; ignore them from one that does. */
void handle_noop(struct wl_client *, struct wl_resource *) {}
void handle_assign(struct wl_client *, struct wl_resource *, struct wl_resource *) {}

const struct ext_workspace_handle_v1_interface handle_impl = {
	.destroy = handle_destroy,
	.activate = handle_activate,
	.deactivate = handle_noop,
	.assign = handle_assign,
	.remove = handle_noop,
};

/* ---- ext_workspace_group_handle_v1 ---- */

void group_resource_destroy(struct wl_resource *resource) {
	if (auto *g = static_cast<WsGroup *>(wl_resource_get_user_data(resource)))
		g->resource = nullptr;
}

void group_create_workspace(struct wl_client *, struct wl_resource *, const char *) {}

void group_destroy(struct wl_client *, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

const struct ext_workspace_group_handle_v1_interface group_impl = {
	.create_workspace = group_create_workspace,
	.destroy = group_destroy,
};

/* A group let go of: its workspaces and outputs no longer point to it
 * (their resources may live on until the client destroys them). With
 * `tell`, the client is told they're gone (a group no longer wanted). */
void free_group(WsGroup *g, bool tell) {
	for (WsHandle *h : g->workspaces) {
		if (h == nullptr)
			continue;
		if (tell) {
			if (g->resource != nullptr)
				ext_workspace_group_handle_v1_send_workspace_leave(g->resource, h->resource);
			ext_workspace_handle_v1_send_removed(h->resource);
		}
		h->group = nullptr;
	}
	for (WsOutput *o : g->outputs) {
		if (tell && g->resource != nullptr)
			ext_workspace_group_handle_v1_send_output_leave(g->resource, o->resource);
		wl_list_remove(&o->destroy.link);
		delete o;
	}
	if (g->resource != nullptr) {
		if (tell)
			ext_workspace_group_handle_v1_send_removed(g->resource);
		wl_resource_set_user_data(g->resource, nullptr);
	}
	delete g;
}

/* ---- ext_workspace_manager_v1 ---- */

void free_client(WsClient *c) {
	auto &v = c->state->clients;
	v.erase(std::remove(v.begin(), v.end(), c), v.end());
	for (WsGroup *g : c->groups)
		free_group(g, false);
	delete c;
}

void manager_resource_destroy(struct wl_resource *resource) {
	if (auto *c = static_cast<WsClient *>(wl_resource_get_user_data(resource)))
		free_client(c);
}

struct wb_output *output_named(struct wb_server *server, const std::string &name) {
	struct wb_output *o;
	wl_list_for_each(o, &server->outputs, link)
		if (o->wlr_output->enabled && name == o->wlr_output->name)
			return o;
	return nullptr;
}

void manager_commit(struct wl_client *, struct wl_resource *resource) {
	auto *c = static_cast<WsClient *>(wl_resource_get_user_data(resource));
	if (c == nullptr)
		return;
	auto pending = std::move(c->pending_activate);
	c->pending_activate.clear();
	struct wb_server *server = c->state->server;
	for (const auto &[output, index] : pending) {
		if (index >= desktop_count(server))
			continue;
		if (output.empty())
			wb_go_to_desktop(server, index);
		else if (struct wb_output *o = output_named(server, output))
			wb_go_to_desktop_on(server, o, index);
	}
}

void manager_stop(struct wl_client *, struct wl_resource *resource) {
	ext_workspace_manager_v1_send_finished(resource);
	wl_resource_destroy(resource);  /* finished is a destructor event */
}

const struct ext_workspace_manager_v1_interface manager_impl = {
	.commit = manager_commit,
	.stop = manager_stop,
};

/* Bring one group up to date; true if anything was sent. */
bool sync_group(WsClient *c, WsGroup *g) {
	struct wb_server *server = c->state->server;
	struct wl_client *client = wl_resource_get_client(c->manager);
	uint32_t version = wl_resource_get_version(c->manager);
	bool changed = false;
	struct wb_output *mine = g->output.empty() ? nullptr : output_named(server, g->output);

	/* Outputs: every one the client bound (shared), or its monitor's. */
	std::vector<struct wl_resource *> current;
	struct wb_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (mine != nullptr && output != mine)
			continue;
		struct wl_resource *r;
		wl_resource_for_each(r, &output->wlr_output->resources) {
			if (wl_resource_get_client(r) == client)
				current.push_back(r);
		}
	}
	for (auto it = g->outputs.begin(); it != g->outputs.end();) {
		WsOutput *o = *it;
		if (std::find(current.begin(), current.end(), o->resource) == current.end()) {
			if (g->resource != nullptr)
				ext_workspace_group_handle_v1_send_output_leave(g->resource, o->resource);
			wl_list_remove(&o->destroy.link);
			delete o;
			it = g->outputs.erase(it);
			changed = true;
		} else {
			++it;
		}
	}
	for (struct wl_resource *r : current) {
		bool known = std::any_of(g->outputs.begin(), g->outputs.end(),
				[r](WsOutput *o) { return o->resource == r; });
		if (known)
			continue;
		auto *o = new WsOutput{};
		o->group = g;
		o->resource = r;
		o->destroy.notify = output_ref_destroyed;
		wl_resource_add_destroy_listener(r, &o->destroy);
		g->outputs.push_back(o);
		if (g->resource != nullptr)
			ext_workspace_group_handle_v1_send_output_enter(g->resource, r);
		changed = true;
	}

	/* Fewer desktops: remove the extra workspaces. */
	int count = desktop_count(server);
	while (static_cast<int>(g->workspaces.size()) > count) {
		WsHandle *h = g->workspaces.back();
		g->workspaces.pop_back();
		if (h != nullptr) {
			if (g->resource != nullptr)
				ext_workspace_group_handle_v1_send_workspace_leave(g->resource, h->resource);
			ext_workspace_handle_v1_send_removed(h->resource);
			h->group = nullptr;  /* inert until the client destroys it */
		}
		changed = true;
	}
	/* More desktops (or a new group): announce workspaces. */
	while (static_cast<int>(g->workspaces.size()) < count) {
		int index = static_cast<int>(g->workspaces.size());
		struct wl_resource *r = wl_resource_create(client,
				&ext_workspace_handle_v1_interface, version, 0);
		if (r == nullptr) {
			wl_client_post_no_memory(client);
			return changed;
		}
		auto *h = new WsHandle{};
		h->group = g;
		h->resource = r;
		h->index = index;
		wl_resource_set_implementation(r, &handle_impl, h, handle_resource_destroy);
		g->workspaces.push_back(h);

		ext_workspace_manager_v1_send_workspace(c->manager, r);
		/* "2"; per monitor "DP-1/2" (ids are unique across groups). */
		std::string id = (g->output.empty() ? "" : g->output + "/") + std::to_string(index + 1);
		ext_workspace_handle_v1_send_id(r, id.c_str());
		struct wl_array coords;
		wl_array_init(&coords);
		if (auto *p = static_cast<uint32_t *>(wl_array_add(&coords, sizeof(uint32_t))))
			*p = static_cast<uint32_t>(index);
		ext_workspace_handle_v1_send_coordinates(r, &coords);
		wl_array_release(&coords);
		ext_workspace_handle_v1_send_capabilities(r,
				EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
		if (g->resource != nullptr)
			ext_workspace_group_handle_v1_send_workspace_enter(g->resource, r);
		changed = true;
	}

	/* Names and the shown desktop (the group's monitor's, or the shared). */
	const wb::DesktopConfig empty;
	const wb::DesktopConfig &desktops = server->config ? server->config->desktops : empty;
	int shown = mine != nullptr ? mine->desktop : server->current_desktop;
	for (WsHandle *h : g->workspaces) {
		if (h == nullptr)
			continue;  /* the client destroyed it; nothing to tell */
		std::string name = wb::desktop_name(desktops, h->index);
		if (name != h->name) {
			h->name = name;
			ext_workspace_handle_v1_send_name(h->resource, name.c_str());
			changed = true;
		}
		uint32_t st = h->index == shown ? EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE : 0;
		/* A window on it asking for attention (Waybar shows it red); per
		 * monitor, one on that monitor. */
		struct wb_toplevel *t;
		wl_list_for_each(t, &server->toplevels, link) {
			if (!t->urgent || !t->mapped)
				continue;
			if (mine != nullptr && get_active_output(t) != mine->wlr_output)
				continue;
			/* One on every desktop: on the one shown, not all of them. */
			if (t->omnipresent ? h->index == shown : t->desktop == h->index)
				st |= EXT_WORKSPACE_HANDLE_V1_STATE_URGENT;
		}
		if (!h->sent_state || st != h->state) {
			h->state = st;
			h->sent_state = true;
			ext_workspace_handle_v1_send_state(h->resource, st);
			changed = true;
		}
	}
	return changed;
}

/* Bring one client up to date: the groups the mode wants (one for all;
 * per monitor, one each), then each group; "done" if anything changed. */
void sync_client(WsClient *c) {
	struct wb_server *server = c->state->server;
	struct wl_client *client = wl_resource_get_client(c->manager);
	uint32_t version = wl_resource_get_version(c->manager);
	bool changed = false;

	std::vector<std::string> want;
	if (desktops_per_monitor(server)) {
		struct wb_output *o;
		wl_list_for_each(o, &server->outputs, link)
			if (o->wlr_output->enabled)
				want.push_back(o->wlr_output->name);
	} else {
		want.push_back("");
	}
	for (auto it = c->groups.begin(); it != c->groups.end();) {
		if (std::find(want.begin(), want.end(), (*it)->output) == want.end()) {
			free_group(*it, true);
			it = c->groups.erase(it);
			changed = true;
		} else {
			++it;
		}
	}
	for (const std::string &name : want) {
		if (std::any_of(c->groups.begin(), c->groups.end(),
				[&name](WsGroup *g) { return g->output == name; }))
			continue;
		struct wl_resource *r = wl_resource_create(client,
				&ext_workspace_group_handle_v1_interface, version, 0);
		if (r == nullptr) {
			wl_client_post_no_memory(client);
			return;
		}
		auto *g = new WsGroup{};
		g->client = c;
		g->output = name;
		g->resource = r;
		wl_resource_set_implementation(r, &group_impl, g, group_resource_destroy);
		c->groups.push_back(g);
		ext_workspace_manager_v1_send_workspace_group(c->manager, r);
		ext_workspace_group_handle_v1_send_capabilities(r, 0);
		changed = true;
	}
	for (WsGroup *g : c->groups)
		changed |= sync_group(c, g);
	if (changed)
		ext_workspace_manager_v1_send_done(c->manager);
}

void manager_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
	auto *st = static_cast<struct wb_workspace_state *>(data);
	struct wl_resource *manager = wl_resource_create(client,
			&ext_workspace_manager_v1_interface, std::min(version, 1u), id);
	if (manager == nullptr) {
		wl_client_post_no_memory(client);
		return;
	}
	auto *c = new WsClient{};
	c->state = st;
	c->manager = manager;
	wl_resource_set_implementation(manager, &manager_impl, c, manager_resource_destroy);
	st->clients.push_back(c);
	sync_client(c);
}

void run_sync(void *data) {
	auto *st = static_cast<struct wb_workspace_state *>(data);
	st->idle = nullptr;
	for (auto it = st->watches.begin(); it != st->watches.end();) {
		if (it->second->dead)
			it = st->watches.erase(it);
		else
			++it;
	}
	std::vector<WsClient *> clients = st->clients;
	for (WsClient *c : clients)
		sync_client(c);
}

void watch_output(struct wb_workspace_state *st, struct wlr_output *wlr_output) {
	auto watch = std::make_unique<OutputWatch>();
	struct wb_server *server = st->server;
	watch->bind.connect(&wlr_output->events.bind,
			[server](void *) { workspace_schedule_sync(server); });
	OutputWatch *w = watch.get();
	watch->destroy.connect(&wlr_output->events.destroy, [server, w](void *) {
		/* Erased in run_sync, not here inside its own callback. */
		w->dead = true;
		w->bind.disconnect();
		w->destroy.disconnect();
		workspace_schedule_sync(server);
	});
	st->watches[wlr_output] = std::move(watch);
}

}  // namespace

void workspace_init(struct wb_server *server) {
	auto *st = new wb_workspace_state{};
	st->server = server;
	server->workspaces = st;
	st->global = wl_global_create(server->wl_display,
			&ext_workspace_manager_v1_interface, 1, st, manager_bind);
	st->new_output.connect(&server->backend->events.new_output,
			[st](void *data) {
		watch_output(st, static_cast<struct wlr_output *>(data));
		workspace_schedule_sync(st->server);
	});
}

void workspace_schedule_sync(struct wb_server *server) {
	struct wb_workspace_state *st = server->workspaces;
	if (st == nullptr || st->idle != nullptr)
		return;
	st->idle = wl_event_loop_add_idle(server->wl_event_loop, run_sync, st);
}

void workspace_finish(struct wb_server *server) {
	struct wb_workspace_state *st = server->workspaces;
	if (st == nullptr)
		return;
	if (st->idle != nullptr)
		wl_event_source_remove(st->idle);
	std::vector<WsClient *> clients = st->clients;
	for (WsClient *c : clients) {
		wl_resource_set_user_data(c->manager, nullptr);
		free_client(c);
	}
	if (st->global != nullptr)
		wl_global_destroy(st->global);
	server->workspaces = nullptr;
	delete st;
}
