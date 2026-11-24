/*
 * Minimal ext-workspace-v1 client (a stand-in for a panel such as Waybar's
 * ext/workspaces) for the workspace integration test.
 *
 * After every manager "done" it prints one line:
 *   done outputs=<n> <id>:<name>:<active|->[:removed] ...
 * in the order the workspaces were announced. Commands on stdin:
 *   activate <id>   activate that workspace and commit
 *   stop            send stop (expects "finished")
 */
#define _GNU_SOURCE
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-workspace-v1-client-protocol.h"

#define MAX_WS 64

struct ws {
	struct ext_workspace_handle_v1 *handle;
	char id[32];
	char name[128];
	uint32_t state;
	int removed;
};

static struct ext_workspace_manager_v1 *manager;
static struct ws workspaces[MAX_WS];
static int num_ws;
static int num_outputs_entered;

static struct ws *find(struct ext_workspace_handle_v1 *h) {
	for (int i = 0; i < num_ws; i++)
		if (workspaces[i].handle == h)
			return &workspaces[i];
	return NULL;
}

static void ws_id(void *data, struct ext_workspace_handle_v1 *h, const char *id) {
	struct ws *w = find(h);
	if (w)
		snprintf(w->id, sizeof(w->id), "%s", id);
}
static void ws_name(void *data, struct ext_workspace_handle_v1 *h, const char *name) {
	struct ws *w = find(h);
	if (w)
		snprintf(w->name, sizeof(w->name), "%s", name);
}
static void ws_coordinates(void *data, struct ext_workspace_handle_v1 *h,
		struct wl_array *coords) {}
static void ws_state(void *data, struct ext_workspace_handle_v1 *h, uint32_t state) {
	struct ws *w = find(h);
	if (w)
		w->state = state;
}
static void ws_capabilities(void *data, struct ext_workspace_handle_v1 *h, uint32_t caps) {}
static void ws_removed(void *data, struct ext_workspace_handle_v1 *h) {
	struct ws *w = find(h);
	if (w) {
		w->removed = 1;
		ext_workspace_handle_v1_destroy(h);
	}
}

static const struct ext_workspace_handle_v1_listener ws_listener = {
	.id = ws_id,
	.name = ws_name,
	.coordinates = ws_coordinates,
	.state = ws_state,
	.capabilities = ws_capabilities,
	.removed = ws_removed,
};

static void group_capabilities(void *data, struct ext_workspace_group_handle_v1 *g, uint32_t c) {}
static void group_output_enter(void *data, struct ext_workspace_group_handle_v1 *g,
		struct wl_output *o) { num_outputs_entered++; }
static void group_output_leave(void *data, struct ext_workspace_group_handle_v1 *g,
		struct wl_output *o) { num_outputs_entered--; }
static void group_workspace_enter(void *data, struct ext_workspace_group_handle_v1 *g,
		struct ext_workspace_handle_v1 *w) {}
static void group_workspace_leave(void *data, struct ext_workspace_group_handle_v1 *g,
		struct ext_workspace_handle_v1 *w) {}
static void group_removed(void *data, struct ext_workspace_group_handle_v1 *g) {}

static const struct ext_workspace_group_handle_v1_listener group_listener = {
	.capabilities = group_capabilities,
	.output_enter = group_output_enter,
	.output_leave = group_output_leave,
	.workspace_enter = group_workspace_enter,
	.workspace_leave = group_workspace_leave,
	.removed = group_removed,
};

static void mgr_workspace_group(void *data, struct ext_workspace_manager_v1 *m,
		struct ext_workspace_group_handle_v1 *g) {
	ext_workspace_group_handle_v1_add_listener(g, &group_listener, NULL);
}
static void mgr_workspace(void *data, struct ext_workspace_manager_v1 *m,
		struct ext_workspace_handle_v1 *h) {
	if (num_ws >= MAX_WS) {
		fprintf(stderr, "too many workspaces\n");
		exit(1);
	}
	memset(&workspaces[num_ws], 0, sizeof(workspaces[num_ws]));
	workspaces[num_ws].handle = h;
	num_ws++;
	ext_workspace_handle_v1_add_listener(h, &ws_listener, NULL);
}
static void mgr_done(void *data, struct ext_workspace_manager_v1 *m) {
	printf("done outputs=%d", num_outputs_entered);
	for (int i = 0; i < num_ws; i++) {
		struct ws *w = &workspaces[i];
		if (w->removed)
			continue;
		printf(" %s:%s:%s", w->id, w->name,
				(w->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) ? "active" : "-");
	}
	printf("\n");
	fflush(stdout);
}
static void mgr_finished(void *data, struct ext_workspace_manager_v1 *m) {
	printf("finished\n");
	fflush(stdout);
	ext_workspace_manager_v1_destroy(m);
	manager = NULL;
}

static const struct ext_workspace_manager_v1_listener mgr_listener = {
	.workspace_group = mgr_workspace_group,
	.workspace = mgr_workspace,
	.done = mgr_done,
	.finished = mgr_finished,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t version) {
	if (strcmp(iface, ext_workspace_manager_v1_interface.name) == 0) {
		manager = wl_registry_bind(reg, name, &ext_workspace_manager_v1_interface, 1);
		ext_workspace_manager_v1_add_listener(manager, &mgr_listener, NULL);
	} else if (strcmp(iface, wl_output_interface.name) == 0) {
		wl_registry_bind(reg, name, &wl_output_interface, 1);
	}
}
static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void command(const char *line) {
	char id[32];
	if (sscanf(line, "activate %31s", id) == 1) {
		for (int i = 0; i < num_ws; i++) {
			if (!workspaces[i].removed && strcmp(workspaces[i].id, id) == 0) {
				ext_workspace_handle_v1_activate(workspaces[i].handle);
				ext_workspace_manager_v1_commit(manager);
				return;
			}
		}
		fprintf(stderr, "no workspace %s\n", id);
	} else if (strncmp(line, "stop", 4) == 0 && manager) {
		ext_workspace_manager_v1_stop(manager);
	}
}

int main(void) {
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "cannot connect\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (manager == NULL) {
		fprintf(stderr, "no ext_workspace_manager_v1\n");
		return 1;
	}
	wl_display_roundtrip(display);

	char buf[256];
	size_t len = 0;
	for (;;) {
		wl_display_flush(display);
		struct pollfd fds[2] = {
			{wl_display_get_fd(display), POLLIN, 0},
			{STDIN_FILENO, POLLIN, 0},
		};
		if (poll(fds, 2, -1) < 0)
			return 1;
		if (fds[0].revents & POLLIN) {
			if (wl_display_dispatch(display) < 0)
				return 1;
		}
		if (fds[0].revents & (POLLHUP | POLLERR))
			return 1;
		if (fds[1].revents & (POLLIN | POLLHUP)) {
			ssize_t n = read(STDIN_FILENO, buf + len, sizeof(buf) - 1 - len);
			if (n <= 0)
				return 0;
			len += (size_t) n;
			buf[len] = '\0';
			char *nl;
			while ((nl = strchr(buf, '\n')) != NULL) {
				*nl = '\0';
				command(buf);
				size_t used = (size_t) (nl + 1 - buf);
				memmove(buf, nl + 1, len - used + 1);
				len -= used;
			}
		}
	}
}
