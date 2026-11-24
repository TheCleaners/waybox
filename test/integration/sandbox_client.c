/*
 * Acts as a sandbox engine for the sandbox integration test: creates a
 * security context (security-context-v1) listening on the Unix socket
 * given as argv[1], marks it with a sandbox engine and app ID, and prints
 * "ready". Clients connecting to that socket are then sandboxed clients.
 * The context closes when this program exits (its close_fd pipe closes);
 * it exits when stdin closes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

#include "security-context-v1-client-protocol.h"

static struct wp_security_context_manager_v1 *manager;

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		const char *iface, uint32_t version) {
	if (strcmp(iface, wp_security_context_manager_v1_interface.name) == 0)
		manager = wl_registry_bind(r, name,
				&wp_security_context_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = {
	.global = reg_global, .global_remove = reg_remove,
};

int main(int argc, char **argv) {
	if (argc != 2) {
		fprintf(stderr, "usage: sandbox_client SOCKET-PATH\n");
		return 2;
	}
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "cannot connect\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);
	if (manager == NULL) {
		fprintf(stderr, "no wp_security_context_manager_v1\n");
		return 1;
	}

	int listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", argv[1]);
	unlink(argv[1]);
	if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 ||
			listen(listen_fd, 4) < 0) {
		perror("socket");
		return 1;
	}
	int close_pipe[2];
	if (pipe2(close_pipe, O_CLOEXEC) < 0) {
		perror("pipe");
		return 1;
	}

	struct wp_security_context_v1 *ctx =
		wp_security_context_manager_v1_create_listener(manager, listen_fd,
				close_pipe[0]);
	close(listen_fd);
	close(close_pipe[0]);
	wp_security_context_v1_set_sandbox_engine(ctx, "org.waybox.test");
	wp_security_context_v1_set_app_id(ctx, "org.waybox.SandboxedApp");
	wp_security_context_v1_set_instance_id(ctx, "1");
	wp_security_context_v1_commit(ctx);
	wl_display_roundtrip(display);
	printf("ready\n");
	fflush(stdout);

	/* Keep the context (and close_pipe[1]) alive until stdin closes. */
	char buf[64];
	while (read(STDIN_FILENO, buf, sizeof(buf)) > 0)
		;
	return 0;
}
