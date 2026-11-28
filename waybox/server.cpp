#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "waybox/output.h"
#include "waybox/stall.hpp"
#include "waybox/render.hpp"
#include "waybox/style.hpp"
#include "waybox/wlroots.hpp"
#include "waybox/wlroots.hpp"
#include "waybox/wlroots.hpp"
#include "waybox/wlroots.hpp"

#include "idle.h"
#include "shortcuts_inhibit.h"
#include "ipc.h"
#include "fixes.h"
#include "toplevel_drag.h"
#include "bell.h"
#include "color_pick.h"
#include "stall_watch.h"
#include "waybox/threads.hpp"
#include "waybox/agent_compositor.hpp"
#include "script.h"
#include "startup_supervisor.h"
#include "waybox/workspace.h"
#include "waybox/xwayland.h"
#include "waybox/pointer_constraints.h"
#include "waybox/sandbox.h"
#include "waybox/window_icon.h"
#include "waybox/desktop_popup.hpp"
#include "waybox/input_method.h"
#include "waybox/server.h"
#include "waybox/session_lock.h"
#include "waybox/frame_view.hpp"
#include "waybox/menu_widget.hpp"
#include "waybox/switcher_widget.hpp"
#include "waybox/about_card.hpp"
#include "waybox/confirm_card.hpp"
#include "waybox/xdg_shell.h"
#include "waybox/xdg_session.h"

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
			/* The compositor blocks the signals it handles on its event loop
			 * (wl_event_loop_add_signal); don't pass that mask on, or the
			 * program could not be interrupted or terminated normally. */
			sigset_t none;
			sigemptyset(&none);
			sigprocmask(SIG_SETMASK, &none, NULL);
			execl("/bin/sh", "/bin/sh", "-c", cmd, (char *) NULL);
			/* Only reached if exec fails. */
			_exit(EXIT_FAILURE);
		}
		_exit(EXIT_SUCCESS);
	}
	/* Reap the intermediate child so it does not linger as a zombie. */
	waitpid(pid, NULL, 0);
}

bool wb_reconfigure(struct wb_server *server, std::string *problem) {
	wb::StallScope timing("reconfigure");
	/* Read the new config first: a broken rc.xml keeps everything as it is. */
	struct wb_config *fresh = reload_config(server, problem);
	if (fresh == nullptr)
		return false;

	/* Drop anything that points into the old config (menu model, key-binding
	 * tree, switcher list) before it is freed. */
	server->menu.reset();
	if (server->icon_cache)
		server->icon_cache->clear();  /* the theme or icon theme may have changed */
	server->switcher.reset();
	server->about_card.reset();
	server->confirm.reset();
	server->desktop_popup.reset();  /* restyled from the new theme */
	server->osd_preview.reset();
	server->switcher_windows.clear();
	server->switcher_mods = 0;
	server->active_keychain = nullptr;
	/* The edge-tiling preview is coloured from the theme: rebuild it. */
	if (server->tile_preview != nullptr) {
		wlr_scene_node_destroy(&server->tile_preview->node);
		server->tile_preview = nullptr;
	}

	deinit_config(server->config);
	/* Desktops added or removed at runtime (AddDesktop, RemoveDesktop)
	 * stay across a reload, unless rc.xml's number changed since: then
	 * that is the user's new choice. */
	if (server->runtime_desktops > 0) {
		if (fresh->desktops.number == server->runtime_desktops_from)
			fresh->desktops.number = server->runtime_desktops;
		else
			server->runtime_desktops = 0;
	}
	server->config = fresh;
	seat_reconfigure_input(server);  /* keymap, <libinput> */
	wb_cursor_apply_theme(server);   /* <cursor> */
	shortcuts_inhibit_update(server);  /* <shortcutsInhibit allow> */

	/* Existing frames were styled from the old theme: rebuild them from the
	 * new one, then re-fit windows (titlebar height may have changed). */
	struct wb_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if (toplevel->frame) {
			server->frame_pressed = nullptr;
			toplevel->frame.reset();
			update_toplevel_decoration(toplevel);
		}
	}
	desktops_reconfigured(server);
	/* <outputs> rules (outputs a client configured keep that), then <hdr>. */
	apply_output_rules(server);
	struct wb_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		apply_output_color(server, output->wlr_output, &state);
		wlr_output_commit_state(output->wlr_output, &state);
		wlr_output_state_finish(&state);
	}
	workspace_schedule_sync(server);  /* names or count may have changed */
	arrange_toplevels(server);
	ipc_reconfigured(server);  /* <ipc>, and tells subscribers */
	scripts_reload(server);    /* scripts are loaded afresh */
	startup_apply(server);     /* <startup>: start, stop what changed */
	idle_reconfigured(server); /* <idle>: the new timeout counts from now */
	wlr_log(WLR_INFO, "%s", _("Reconfigured"));
	seat_recheck_keyboard(server);  /* <virtualInput> may have changed */
	return true;
}

/* Colour management (color-management-v1, color-representation-v1): clients
 * describe their colour spaces, and the scene converts them for each
 * output. Needs a renderer that applies colour transforms (Vulkan). */
/* The renderer: Vulkan by default when Waybox drives real displays (a DRM
 * backend), since colour management and HDR need it. Nested and headless
 * sessions keep wlroots' choice, since there Vulkan may pick another GPU
 * than the one the output buffers come from. WLR_RENDERER overrides
 * either; without Vulkan, the usual choice. */
static struct wlr_renderer *create_renderer(struct wb_server *server) {
	struct wlr_renderer *renderer = nullptr;
	if (getenv("WLR_RENDERER") == nullptr &&
			wlr_backend_get_drm_fd(server->backend) >= 0) {
		setenv("WLR_RENDERER", "vulkan", 1);
		renderer = wlr_renderer_autocreate(server->backend);
		unsetenv("WLR_RENDERER");
		if (renderer == nullptr)
			wlr_log(WLR_INFO, "%s", _("Vulkan renderer unavailable; using the default"));
	}
	if (renderer == nullptr)
		renderer = wlr_renderer_autocreate(server->backend);
	return renderer;
}

static void watch_renderer(struct wb_server *server);

/* A GPU reset (some drivers on resume, a hang recovered): wlroots says
 * the renderer is lost, and everything drawn with it is gone. As sway
 * does: a new renderer and allocator, given to the compositor (new
 * buffers) and every output, then the old ones go. Window captures were
 * made with the old ones: they end (capturing programs ask again). */
static void renderer_lost(struct wb_server *server) {
	wlr_log(WLR_INFO, "%s", _("The GPU was reset: re-creating the renderer"));
	struct wlr_renderer *renderer = create_renderer(server);
	if (renderer == nullptr) {
		wlr_log(WLR_ERROR, "%s", _("Failed to re-create the renderer"));
		return;
	}
	struct wlr_allocator *allocator = wlr_allocator_autocreate(server->backend, renderer);
	if (allocator == nullptr) {
		wlr_log(WLR_ERROR, "%s", _("Failed to re-create the allocator"));
		wlr_renderer_destroy(renderer);
		return;
	}
	struct wb_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link)
		drop_window_capture(t);
	struct wlr_renderer *old_renderer = server->renderer;
	struct wlr_allocator *old_allocator = server->allocator;
	server->renderer = renderer;
	server->allocator = allocator;
	watch_renderer(server);  /* (off the old one) */
	wlr_compositor_set_renderer(server->compositor, renderer);
	/* Each monitor to the new one; one that can't take it (its backend and
	 * the new buffers don't match) still points at the old: it's turned
	 * off, and the old renderer kept for as long as that lasts (freeing it
	 * now, its next frame would use freed memory). */
	bool all = true;
	struct wb_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (!wb_test_fail_once("init_render") &&
				wlr_output_init_render(output->wlr_output, allocator, renderer))
			continue;
		all = false;
		wlr_log(WLR_ERROR, "wb-renderer %s can't render with the new renderer: turned off",
				output->wlr_output->name);
		struct wlr_output_state off;
		wlr_output_state_init(&off);
		wlr_output_state_set_enabled(&off, false);
		wlr_output_commit_state(output->wlr_output, &off);
		wlr_output_state_finish(&off);
	}
	if (all) {
		wlr_allocator_destroy(old_allocator);
		wlr_renderer_destroy(old_renderer);
	} else {
		server->old_renderers.emplace_back(old_renderer, old_allocator);
		output_manager_update_config(server);  /* tools see it off */
	}
	/* What windows showed was on the GPU, and is gone: ask each to draw
	 * again (a configure; toolkits redraw on it). */
	wl_list_for_each(t, &server->toplevels, link)
		if (t->mapped && t->kind == wb_view_kind::Xdg && t->xdg_toplevel != nullptr)
			wlr_xdg_surface_schedule_configure(t->xdg_toplevel->base);
	wlr_log(WLR_INFO, "wb-renderer re-created");
}

/* The work runs once the signal is over: the old renderer can't be
 * destroyed while it's emitting "lost" (its listener list isn't empty
 * then). Several "lost" in a row make one re-creation. */
static struct wl_event_source *renderer_lost_idle = nullptr;

static void watch_renderer(struct wb_server *server) {
	server->renderer_lost.connect(&server->renderer->events.lost, [server](void *) {
		if (renderer_lost_idle != nullptr)
			return;
		renderer_lost_idle = wl_event_loop_add_idle(server->wl_event_loop, [](void *data) {
			renderer_lost_idle = nullptr;
			renderer_lost(static_cast<struct wb_server *>(data));
		}, server);
	});
}

bool wb_test_renderer_lost(struct wb_server *server) {
	const char *on = getenv("WAYBOX_TEST_GPU_RESET");
	if (on == nullptr || strcmp(on, "1") != 0 || server->renderer == nullptr)
		return false;
	wl_signal_emit_mutable(&server->renderer->events.lost, nullptr);
	return true;
}

static void init_color_management(struct wb_server *server) {
#if WLR_CHECK_VERSION(0, 20, 0)
	if (!server->renderer->features.input_color_transform) {
		wlr_log(WLR_INFO, "%s", _("Color management needs the Vulkan renderer"));
		return;
	}
	const enum wp_color_manager_v1_render_intent intents[] = {
		WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL,
	};
	size_t n_tf = 0, n_primaries = 0;
	enum wp_color_manager_v1_transfer_function *tfs =
			wlr_color_manager_v1_transfer_function_list_from_renderer(
					server->renderer, &n_tf);
	enum wp_color_manager_v1_primaries *primaries =
			wlr_color_manager_v1_primaries_list_from_renderer(
					server->renderer, &n_primaries);
	struct wlr_color_manager_v1_options options = {};
	options.features.parametric = true;
	options.features.set_mastering_display_primaries = true;
	options.render_intents = intents;
	options.render_intents_len = sizeof(intents) / sizeof(intents[0]);
	options.transfer_functions = tfs;
	options.transfer_functions_len = n_tf;
	options.primaries = primaries;
	options.primaries_len = n_primaries;
	server->color_manager = wlr_color_manager_v1_create(server->wl_display, 1, &options);
	free(tfs);
	free(primaries);
	wlr_color_representation_manager_v1_create_with_renderer(server->wl_display, 1,
			server->renderer);
	wlr_log(WLR_INFO, "%s", _("Color management enabled"));
#else
	(void) server;
#endif
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
	server->renderer = create_renderer(server);
	if (server->renderer == NULL) {
		wlr_log(WLR_ERROR, "%s", _("Failed to create renderer"));
		return false;
	}

	wlr_renderer_init_wl_display(server->renderer, server->wl_display);
	init_color_management(server);
	init_sandbox(server);

	/* Explicit sync (linux-drm-syncobj-v1): clients pass GPU fences with
	 * their buffers instead of relying on implicit sync, which NVIDIA's
	 * driver does not provide. The scene graph waits on and signals the
	 * timelines itself. Needs both the renderer and the backend to handle
	 * timelines (the DRM backend does; headless does not). */
	if (server->renderer->features.timeline &&
			server->backend->features.timeline) {
		int drm_fd = wlr_renderer_get_drm_fd(server->renderer);
		if (drm_fd >= 0 && wlr_linux_drm_syncobj_manager_v1_create(
				server->wl_display, 1, drm_fd) != NULL)
			wlr_log(WLR_INFO, "%s", _("Explicit sync (linux-drm-syncobj-v1) enabled"));
	} else {
		wlr_log(WLR_INFO, "%s", _("Explicit sync unavailable with this renderer/backend"));
	}

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
	watch_renderer(server);
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
	seat_init_test_touch(server);

	server->output_manager = wlr_xdg_output_manager_v1_create(server->wl_display,
				server->output_layout);

	return true;
}

bool wb_start_server(struct wb_server* server) {
	init_config(server);
	wb_cursor_apply_theme(server);  /* <cursor>, before anything shows it */
	/* Initialise the toplevel list before the backend starts, since bringing
	 * up outputs can trigger arrange_toplevels() before xdg-shell is set up. */
	wl_list_init(&server->toplevels);
	wl_list_init(&server->focus_order);
	init_output(server);
	workspace_init(server);  /* after init_output: watches outputs it adds */

	/* Create a scene graph. This is a wlroots abstraction that handles all
	 * rendering and damage tracking. All the compositor author needs to do
	 * is add things that should be rendered to the scene graph at the proper
	 * positions and then call wlr_scene_output_commit() to render a frame if
	 * necessary.
	 */
	server->scene = wlr_scene_create();
#if WLR_CHECK_VERSION(0, 20, 0)
	if (server->color_manager != nullptr)
		wlr_scene_set_color_manager_v1(server->scene, server->color_manager);
#endif
	server->scene_layout =
		wlr_scene_attach_output_layout(server->scene, server->output_layout);
	/* The toplevel container. Created up-front so per-output layer trees (made
	 * as outputs come up) can be stacked relative to it; toplevels are added as
	 * its children, so adding/raising a window never lifts it above the panel
	 * layers. */
	server->toplevel_tree = wlr_scene_tree_create(&server->scene->tree);
	for (struct wlr_scene_tree *&layer : server->toplevel_layers)
		layer = wlr_scene_tree_create(server->toplevel_tree);
	server->unmanaged_tree = wlr_scene_tree_create(&server->scene->tree);
	server->drag_icon_tree = wlr_scene_tree_create(&server->scene->tree);

	const char *socket = wl_display_add_socket_auto(server->wl_display);
	/* On failure the caller runs wb_terminate(), which frees the backend
	 * and the display: freeing them here too was a double free. */
	if (!socket)
		return false;
	/* The scripting socket with it, named after it: whatever sees the
	 * display (a test, the autostart script) can reach it too. */
	setenv("WAYLAND_DISPLAY", socket, true);
	ipc_reconfigured(server);

	if (!wlr_backend_start(server->backend)) {
		wlr_log(WLR_ERROR, "%s", _("Failed to start backend"));
		return false;
	}

	wlr_log(WLR_INFO, "%s: WAYLAND_DISPLAY=%s", _("Running Wayland compositor on Wayland display"), socket);
	setenv("WAYLAND_DISPLAY", socket, true);
	/* For scripts that signal the compositor (waybox-menu's Exit and
	 * Reconfigure): pgrep would also find the session's shell wrapper. */
	setenv("WAYBOX_PID", std::to_string(getpid()).c_str(), true);

	wlr_ext_data_control_manager_v1_create(server->wl_display, 1);
	/* The older wlr protocol, which clipboard managers such as cliphist and
	 * clipman (and older wl-clipboard) still speak. Privileged, like the
	 * ext one: sandboxed clients don't see it. */
	wlr_data_control_manager_v1_create(server->wl_display);
	wlr_data_device_manager_create(server->wl_display);

	server->foreign_toplevel_list =
		wlr_ext_foreign_toplevel_list_v1_create(server->wl_display, 1);
	server->foreign_toplevel_manager =
		wlr_foreign_toplevel_manager_v1_create(server->wl_display);

	/* xdg-activation-v1: a client (or launcher) can ask to activate/raise a
	 * surface, e.g. to bring a just-launched window to the front. */
	server->xdg_activation = wlr_xdg_activation_v1_create(server->wl_display);
	server->request_activate.connect(
			&server->xdg_activation->events.request_activate,
			[](void *data) {
		auto *event =
			static_cast<struct wlr_xdg_activation_v1_request_activate_event *>(data);
		/* On behalf of the user: a token made from an input event (it has
		 * a seat, and wlroots checked its serial), as when you click a link
		 * or launch something. A program on its own can't take the
		 * keyboard (or switch desktops): its window is marked urgent. */
		bool from_user = event->token != nullptr && event->token->seat != nullptr;
		struct wlr_xdg_surface *xdg_surface =
			wlr_xdg_surface_try_from_wlr_surface(event->surface);
		if (xdg_surface == NULL ||
				xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL ||
				!xdg_surface->surface->mapped)
			return;
		auto *tree = static_cast<struct wlr_scene_tree *>(xdg_surface->data);
		struct wb_toplevel *toplevel =
			toplevel_from_node(tree ? &tree->node : NULL);
		if (toplevel != NULL)
			toplevel_request_activation(toplevel, from_user);
	});

	server->gamma_control_manager =
		wlr_gamma_control_manager_v1_create(server->wl_display);
	/* The scene applies gamma (night light: wlsunset, gammastep) on each
	 * output's commit. */
	wlr_scene_set_gamma_control_manager_v1(server->scene, server->gamma_control_manager);

	wlr_ext_image_copy_capture_manager_v1_create(server->wl_display, 1);
	wlr_ext_output_image_capture_source_manager_v1_create(server->wl_display, 1);
	/* wlr-screencopy: the older capture protocol, for tools that only know
	 * it (some screen recorders, older grim and wayvnc). Sandboxed
	 * programs don't get it (security.cpp). */
	wlr_screencopy_manager_v1_create(server->wl_display);
	init_window_capture(server);
	create_idle_manager(server);

	init_xdg_decoration(server);
	init_layer_shell(server);
	init_session_lock(server);

	/* Set up the xdg-shell. The xdg-shell is a Wayland protocol which is used
	 * for application windows. For more detail on shells, refer to
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	init_xdg_shell(server);
	init_xwayland(server);  /* after the seat, cursor and config */
	init_pointer_constraints(server);  /* after the seat and cursor */
	init_shortcuts_inhibit(server);    /* after the seat */
	init_fixes(server);
	init_toplevel_drag(server);
	init_xdg_session(server);  /* session restore */
	init_bell(server);
	init_stall_watch(server);
	/* Threads: before what may hand work to them (scripts, menus). */
	server->main_queue = std::make_shared<wb::MainQueue>();
	server->main_queue_source = wl_event_loop_add_fd(
			wl_display_get_event_loop(server->wl_display), server->main_queue->fd(),
			WL_EVENT_READABLE, [](int, uint32_t, void *data) {
				static_cast<wb::MainQueue *>(data)->run();
				return 0;
			}, server->main_queue.get());
	server->workers = std::make_unique<wb::Workers>();
	server->icon_cache = std::make_unique<wb::IconCache>();
	/* Fonts warmed once Waybox is idle after starting: their first use costs
	 * fontconfig's and pango's setup (some 15 ms), which would otherwise
	 * fall on the first menu opened. */
	wl_event_loop_add_idle(wl_display_get_event_loop(server->wl_display), [](void *data) {
		auto *server = static_cast<struct wb_server *>(data);
		if (server->config == nullptr)
			return;
		wb::StallScope timing("font-warmup");
		wb::measure_text("Ag", wb::menu_style_from_theme(server->config->theme).item_text.font);
	}, server);
	agent_compositor_init(server);
	init_input_method(server);
	init_scripts(server);  /* last: they may act on all of the above */

	wlr_fractional_scale_manager_v1_create(server->wl_display, 1);
	wlr_viewporter_create(server->wl_display);

	/* Lightweight client-facing protocols:
	 * - presentation-time: lets clients (media players, games) learn when their
	 *   frames are actually shown, for smooth A/V sync and vsync timing.
	 * - single-pixel-buffer: a cheap solid-colour 1x1 buffer source.
	 * - content-type: clients hint their content (e.g. "video", "game") so the
	 *   compositor can adapt. */
	wlr_presentation_create(server->wl_display, server->backend, 2);
	wlr_single_pixel_buffer_manager_v1_create(server->wl_display);
	/* alpha-modifier: a client sets a surface's opacity (the scene draws it). */
	wlr_alpha_modifier_v1_create(server->wl_display);
	wlr_content_type_manager_v1_create(server->wl_display, 1);

	init_window_icons(server);  /* xdg-toplevel-icon */
	init_toplevel_tags(server);  /* xdg-toplevel-tag */
	/* Modal dialogs (xdg-dialog), and dialogs of other programs, such as a
	 * portal's file chooser, attached to a window (xdg-foreign v1 for GTK 3,
	 * v2 for GTK 4): wlroots sets the parent; Waybox centres and stacks. */
	wlr_xdg_wm_dialog_v1_create(server->wl_display, 1);
	struct wlr_xdg_foreign_registry *foreign = wlr_xdg_foreign_registry_create(server->wl_display);
	wlr_xdg_foreign_v1_create(server->wl_display, foreign);
	wlr_xdg_foreign_v2_create(server->wl_display, foreign);

	return true;
}

bool wb_terminate(struct wb_server* server) {
	/* Destroy all clients first, so that surface unmap/destroy handlers (which
	 * reference server->cursor, server->seat and server->config) run while
	 * those objects are still alive. */
	startup_restore_env(server);  /* before the programs end */
	startup_finish(server);  /* end the <startup> programs */
	ipc_finish(server);
	finish_idle(server);
	finish_xdg_session(server);  /* saves the windows while they're there */
	wl_display_destroy_clients(server->wl_display);
	finish_session_lock(server);
	workspace_finish(server);
	finish_shortcuts_inhibit(server);
	finish_xwayland(server);  /* X11 windows still use the seat and cursor */
	agent_compositor_finish(server);  /* its thread posts to the queue */
	server->menu.reset();  /* (before the icons it holds references to) */
	server->icon_cache.reset();
	/* First: the workers finish (or give up) and stop, then results still
	 * waiting are dropped, before what they'd have been applied to goes. */
	server->workers.reset();
	if (server->main_queue_source != nullptr)
		wl_event_source_remove(server->main_queue_source);
	server->main_queue_source = nullptr;
	if (server->main_queue)
		server->main_queue->close();
	finish_pointer_constraints(server);
	finish_input_method(server);

	server->desktop_popup.reset();  /* its timer is on the event loop */
	server->osd_preview.reset();
	server->identify_popups.clear();
	finish_color_pick(server);
	finish_bell(server);  /* its flashes' timers too */
	finish_stall_watch(server);
	finish_scripts(server);  /* its timers are on the event loop */
	seat_finish_test_touch(server);
	server->cursor.reset();
	server->new_xdg_decoration.disconnect(); /* wb_decoration_destroy */
	deinit_config(server->config);
	server->output_layout_change.disconnect();  /* before its signal is freed */
	wlr_output_layout_destroy(server->output_layout);
	wlr_allocator_destroy(server->allocator);
	server->renderer_lost.disconnect();  /* the renderer asserts on it */
	if (renderer_lost_idle != nullptr) {
		wl_event_source_remove(renderer_lost_idle);
		renderer_lost_idle = nullptr;
	}
	wlr_renderer_destroy(server->renderer);
	for (auto &[r, a] : server->old_renderers) {
		wlr_allocator_destroy(a);
		wlr_renderer_destroy(r);
	}
	server->old_renderers.clear();

	server->new_input.disconnect();
	server->new_virtual_keyboard.disconnect();
	server->set_toplevel_icon.disconnect();
	server->new_virtual_pointer.disconnect();
	server->request_activate.disconnect();
	server->new_output.disconnect();
	server->output_configuration_applied.disconnect();
	server->output_configuration_tested.disconnect();
	server->output_power_set_mode.disconnect();
	server->drm_lease_request.disconnect();
	server->new_inhibitor.disconnect();
	server->destroy_inhibit_manager.disconnect();
	server->new_layer_surface.disconnect();

	server->new_xdg_toplevel.disconnect();
	server->new_window_capture.disconnect();
	server->new_xdg_popup.disconnect();

	wlr_backend_destroy(server->backend);
	server->menu.reset();  /* tear down any open menu before its scene tree */
	server->about_card.reset();
	server->confirm.reset();
	server->seat.reset();
	wl_display_destroy(server->wl_display);
	wlr_scene_node_destroy(&server->scene->tree.node);

	wlr_log(WLR_INFO, "%s", _("Display destroyed"));

	return true;
}
