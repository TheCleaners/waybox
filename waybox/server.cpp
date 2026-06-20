#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "waybox/wlroots.hpp"
#include "waybox/wlroots.hpp"
#include "waybox/wlroots.hpp"
#include "waybox/wlroots.hpp"

#include "idle.h"
#include "waybox/server.h"
#include "waybox/xdg_shell.h"

void wb_spawn(const char *cmd) {
	if (cmd == NULL || cmd[0] == '\0') {
		return;
	}

	/* Double-fork so the spawned process is reparented to init and never
	 * becomes a zombie of the compositor, which does not reap children. */
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log(WLR_ERROR, "fork failed: %s", strerror(errno));
		return;
	}
	if (pid == 0) {
		setsid();
		pid_t grandchild = fork();
		if (grandchild < 0) {
			_exit(EXIT_FAILURE);
		}
		if (grandchild == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, (char *) NULL);
			/* Only reached if exec fails. */
			_exit(EXIT_FAILURE);
		}
		_exit(EXIT_SUCCESS);
	}
	/* Reap the intermediate child so it does not linger as a zombie. */
	waitpid(pid, NULL, 0);
}

bool wb_create_backend(struct wb_server* server) {
	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, manging Wayland globals, and so on. */
	server->wl_display = wl_display_create();
	if (server->wl_display == NULL) {
		wlr_log(WLR_ERROR, "%s", _("Failed to connect to a Wayland display"));
		return false;
	}

	server->wl_event_loop = wl_display_get_event_loop(server->wl_display);
	if (server->wl_event_loop == NULL) {
		wlr_log(WLR_ERROR, "%s", _("Failed to get an event loop"));
		return false;
	}

	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. */
	server->backend = wlr_backend_autocreate(server->wl_event_loop, &server->session);
	if (server->backend == NULL) {
		wlr_log(WLR_ERROR, "%s", _("Failed to create backend"));
		return false;
	}

	/* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
         * can also specify a renderer using the WLR_RENDERER env var.
         * The renderer is responsible for defining the various pixel formats it
         * supports for shared memory, this configures that for clients. */
	server->renderer = wlr_renderer_autocreate(server->backend);
	if (server->renderer == NULL) {
		wlr_log(WLR_ERROR, "%s", _("Failed to create renderer"));
		return false;
	}

	wlr_renderer_init_wl_display(server->renderer, server->wl_display);

        /* Autocreates an allocator for us.
         * The allocator is the bridge between the renderer and the backend. It
         * handles the buffer creation, allowing wlroots to render onto the
         * screen */
        server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
	if (server->allocator == NULL) {
		wlr_log(WLR_ERROR, "%s", _("Failed to create allocator"));
		return false;
	}

	server->compositor =
		wlr_compositor_create(server->wl_display, 5, server->renderer);
	server->subcompositor = wlr_subcompositor_create(server->wl_display);
	server->output_layout = wlr_output_layout_create(server->wl_display);
	server->seat.reset(wb_seat_create(server));
	if (server->seat == nullptr) {
		wlr_log(WLR_ERROR, "%s", _("Failed to create seat"));
		return false;
	}
	server->cursor = wb_cursor_create(server);
	if (server->cursor == nullptr) {
		wlr_log(WLR_ERROR, "%s", _("Failed to create cursor"));
		return false;
	}

	server->output_manager = wlr_xdg_output_manager_v1_create(server->wl_display,
				server->output_layout);

	return true;
}

bool wb_start_server(struct wb_server* server) {
	init_config(server);
	/* Initialise the toplevel list before the backend starts, since bringing
	 * up outputs can trigger arrange_toplevels() before xdg-shell is set up. */
	wl_list_init(&server->toplevels);
	wl_list_init(&server->focus_order);
	init_output(server);

	/* Create a scene graph. This is a wlroots abstraction that handles all
	 * rendering and damage tracking. All the compositor author needs to do
	 * is add things that should be rendered to the scene graph at the proper
	 * positions and then call wlr_scene_output_commit() to render a frame if
	 * necessary.
	 */
	server->scene = wlr_scene_create();
	server->scene_layout =
		wlr_scene_attach_output_layout(server->scene, server->output_layout);

	const char *socket = wl_display_add_socket_auto(server->wl_display);
	if (!socket) {
		wlr_backend_destroy(server->backend);
		return false;
	}

	if (!wlr_backend_start(server->backend)) {
		wlr_log(WLR_ERROR, "%s", _("Failed to start backend"));
		wlr_backend_destroy(server->backend);
		wl_display_destroy(server->wl_display);
		return false;
	}

	wlr_log(WLR_INFO, "%s: WAYLAND_DISPLAY=%s", _("Running Wayland compositor on Wayland display"), socket);
	setenv("WAYLAND_DISPLAY", socket, true);

	wlr_ext_data_control_manager_v1_create(server->wl_display, 1);
	wlr_data_device_manager_create(server->wl_display);

	server->foreign_toplevel_list =
		wlr_ext_foreign_toplevel_list_v1_create(server->wl_display, 1);

	server->gamma_control_manager =
		wlr_gamma_control_manager_v1_create(server->wl_display);
	server->gamma_control_set_gamma.connect(&server->gamma_control_manager->events.set_gamma,
			[server](void *data) { handle_gamma_control_set_gamma(server, data); });

	wlr_ext_image_copy_capture_manager_v1_create(server->wl_display, 1);
	wlr_ext_output_image_capture_source_manager_v1_create(server->wl_display, 1);
	create_idle_manager(server);

	init_xdg_decoration(server);
	init_layer_shell(server);

	/* Set up the xdg-shell. The xdg-shell is a Wayland protocol which is used
	 * for application windows. For more detail on shells, refer to
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	init_xdg_shell(server);

	wlr_fractional_scale_manager_v1_create(server->wl_display, 1);
	wlr_viewporter_create(server->wl_display);

	struct wlr_xdg_toplevel_icon_manager_v1 * icon_manager = wlr_xdg_toplevel_icon_manager_v1_create(server->wl_display, 1);
	int sizes[] = {16, 24, 32, 48, 64};
	wlr_xdg_toplevel_icon_manager_v1_set_sizes(icon_manager, (int *) sizes, 5);

	return true;
}

bool wb_terminate(struct wb_server* server) {
	/* Destroy all clients first, so that surface unmap/destroy handlers (which
	 * reference server->cursor, server->seat and server->config) run while
	 * those objects are still alive. */
	wl_display_destroy_clients(server->wl_display);

	server->cursor.reset();
	server->new_xdg_decoration.disconnect(); /* wb_decoration_destroy */
	deinit_config(server->config);
	wlr_output_layout_destroy(server->output_layout);
	wlr_allocator_destroy(server->allocator);
	wlr_renderer_destroy(server->renderer);

	server->new_input.disconnect();
	server->new_virtual_keyboard.disconnect();
	server->new_virtual_pointer.disconnect();
	server->new_output.disconnect();
	server->output_configuration_applied.disconnect();
	server->output_configuration_tested.disconnect();
	server->new_inhibitor.disconnect();
	wl_list_remove(&server->inhibitors);
	server->destroy_inhibit_manager.disconnect();
	server->gamma_control_set_gamma.disconnect();
	server->new_layer_surface.disconnect();

	server->new_xdg_toplevel.disconnect();
	server->new_xdg_popup.disconnect();

	wlr_backend_destroy(server->backend);
	server->seat.reset();
	wl_display_destroy(server->wl_display);
	wlr_scene_node_destroy(&server->scene->tree.node);

	wlr_log(WLR_INFO, "%s", _("Display destroyed"));

	return true;
}
