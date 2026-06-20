#include <math.h>

#include "waybox/wlroots.hpp"

#include "waybox/output.h"

/* Pick a sensible default scale for a freshly connected output based on its
 * pixel density. 96 dpi maps to 1.0; the result is rounded to the nearest
 * quarter step and clamped. This is only an initial guess: the user can
 * override it at any time through the output-management protocol (wlr-randr,
 * kanshi, way-displays, ...). */
static float compute_default_scale(struct wlr_output *output,
		struct wlr_output_mode *mode) {
	int width = mode ? mode->width : output->width;
	int height = mode ? mode->height : output->height;
	if (output->phys_width <= 0 || output->phys_height <= 0 ||
			width <= 0 || height <= 0) {
		return 1.0f;
	}
	double diag_px = sqrt((double) width * width + (double) height * height);
	double diag_mm = sqrt(
			(double) output->phys_width * output->phys_width +
			(double) output->phys_height * output->phys_height);
	double diag_in = diag_mm / 25.4;
	if (diag_in <= 0.0) {
		return 1.0f;
	}
	double dpi = diag_px / diag_in;
	double scale = round(dpi / 96.0 * 4.0) / 4.0;
	if (scale < 1.0) {
		scale = 1.0;
	} else if (scale > 3.0) {
		scale = 3.0;
	}
	return (float) scale;
}

/* Recompute derived state after an output's configuration changes: refresh the
 * cached layout geometry, re-arrange layer surfaces (which updates the usable
 * area) and reflow toplevels so maximized/fullscreen windows track the new
 * size and floating windows stay reachable. */
static void reconfigure_output(struct wb_output *output) {
	if (output == NULL) {
		return;
	}
	wlr_output_layout_get_box(output->server->output_layout,
			output->wlr_output, &output->geometry);
	arrange_layers(output);
	arrange_toplevels(output->server);
}

/* Advertise the current output configuration to output-management clients. */
static void output_manager_update_config(struct wb_server *server) {
	struct wlr_output_configuration_v1 *config =
		wlr_output_configuration_v1_create();
	struct wb_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output_configuration_head_v1 *head =
			wlr_output_configuration_head_v1_create(config, output->wlr_output);
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
		if (!wlr_box_empty(&box)) {
			head->state.x = box.x;
			head->state.y = box.y;
		}
	}
	wlr_output_manager_v1_set_configuration(server->wlr_output_manager, config);
}

/* Apply (or test) an output configuration requested through the
 * output-management protocol. */
static bool apply_output_config(struct wb_server *server,
		struct wlr_output_configuration_v1 *config, bool test_only) {
	bool ok = true;
	struct wlr_output_configuration_head_v1 *head;
	wl_list_for_each(head, &config->heads, link) {
		struct wlr_output *wlr_output = head->state.output;
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, head->state.enabled);
		if (head->state.enabled) {
			if (head->state.mode != NULL) {
				wlr_output_state_set_mode(&state, head->state.mode);
			} else {
				wlr_output_state_set_custom_mode(&state,
						head->state.custom_mode.width,
						head->state.custom_mode.height,
						head->state.custom_mode.refresh);
			}
			wlr_output_state_set_scale(&state, head->state.scale);
			wlr_output_state_set_transform(&state, head->state.transform);
			wlr_output_state_set_adaptive_sync_enabled(&state,
					head->state.adaptive_sync_enabled);
		}
		if (test_only) {
			ok = wlr_output_test_state(wlr_output, &state) && ok;
		} else {
			ok = wlr_output_commit_state(wlr_output, &state) && ok;
		}
		wlr_output_state_finish(&state);
	}

	if (test_only || !ok) {
		return ok;
	}

	wl_list_for_each(head, &config->heads, link) {
		struct wlr_output *wlr_output = head->state.output;
		if (head->state.enabled) {
			wlr_output_layout_add(server->output_layout, wlr_output,
					head->state.x, head->state.y);
			reconfigure_output(static_cast<wb_output *>(wlr_output->data));
		} else {
			wlr_output_layout_remove(server->output_layout, wlr_output);
		}
	}
	return ok;
}

void wb_output::on_frame(void *data) {
	/* Called every time the output is ready to display a frame, generally at
	 * the output's refresh rate (e.g. 60Hz). */
	wb_output *output = this;
	struct wlr_scene *scene = output->server->scene;
	struct wlr_scene_output *scene_output =
		wlr_scene_get_scene_output(scene, output->wlr_output);

	wlr_output_layout_get_box(output->server->output_layout,
			output->wlr_output, &output->geometry);

	if (output->gamma_lut_changed) {
		output->gamma_lut_changed = false;
		struct wlr_gamma_control_v1 *gamma_control =
			wlr_gamma_control_manager_v1_get_control(output->server->gamma_control_manager,
					output->wlr_output);
		struct wlr_output_state pending;
		if (!wlr_scene_output_build_state(scene_output, &pending, NULL))
			return;

		if (!wlr_gamma_control_v1_apply(gamma_control, &pending)) {
			wlr_output_state_finish(&pending);
			return;
		}

		if (!wlr_output_test_state(output->wlr_output, &pending)) {
			wlr_gamma_control_v1_send_failed_and_destroy(gamma_control);
			wlr_output_state_finish(&pending);
			return;
		}

		wlr_output_state_finish(&pending);
	}

	/* Render the scene if needed and commit the output */
	wlr_scene_output_commit(scene_output, NULL);

	/* This lets the client know that we've displayed that frame and it can
	 * prepare another one now if it likes. */
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

void output_configuration_applied(struct wb_server *server, void *data) {
	struct wlr_output_configuration_v1 *configuration = static_cast<struct wlr_output_configuration_v1 *>(data);
	if (apply_output_config(server, configuration, false)) {
		wlr_output_configuration_v1_send_succeeded(configuration);
	} else {
		wlr_output_configuration_v1_send_failed(configuration);
	}
	wlr_output_configuration_v1_destroy(configuration);
	output_manager_update_config(server);
}

void output_configuration_tested(struct wb_server *server, void *data) {
	struct wlr_output_configuration_v1 *configuration = static_cast<struct wlr_output_configuration_v1 *>(data);
	if (apply_output_config(server, configuration, true)) {
		wlr_output_configuration_v1_send_succeeded(configuration);
	} else {
		wlr_output_configuration_v1_send_failed(configuration);
	}
	wlr_output_configuration_v1_destroy(configuration);
}

void wb_output::on_request_state(void *data) {
	auto *event = static_cast<const struct wlr_output_event_request_state *>(data);

	if (wlr_output_commit_state(wlr_output, event->state)) {
		reconfigure_output(this);
		output_manager_update_config(server);
	}
}

void handle_gamma_control_set_gamma(struct wb_server *server, void *data) {
	const struct wlr_gamma_control_manager_v1_set_gamma_event *event = static_cast<const struct wlr_gamma_control_manager_v1_set_gamma_event *>(data);
	struct wb_output *output = static_cast<struct wb_output *>(event->output->data);
	output->gamma_lut_changed = true;
	wlr_output_schedule_frame(output->wlr_output);
}

void new_output_notify(struct wb_server *server, void *data) {
	struct wlr_output *wlr_output = static_cast<struct wlr_output *>(data);
	wlr_log(WLR_INFO, "%s: %s", _("New output device detected"), wlr_output->name);

	/* Configures the output created by the backend to use our allocator
         * and our renderer */
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}

	/* Choose an initial scale based on the display's pixel density. */
	wlr_output_state_set_scale(&state, compute_default_scale(wlr_output, mode));

	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	auto *output = new wb_output{};
	output->server = server;
	output->wlr_output = wlr_output;
	wlr_output->data = output;

	/* Initializes the layers */
	size_t num_layers = sizeof(output->layers) / sizeof(struct wlr_scene_node *);
	for (size_t i = 0; i < num_layers; i++) {
		((struct wlr_scene_node **) &output->layers)[i] =
			&wlr_scene_tree_create(&server->scene->tree)->node;
	}

	wl_list_insert(&server->outputs, &output->link);

	/* Initialise the usable area to the full output; arrange_layers() will
	 * shrink it once layer-shell clients (panels) reserve exclusive zones. */
	wlr_output_effective_resolution(wlr_output,
			&output->usable_area.width, &output->usable_area.height);

	output->frame.connect(&wlr_output->events.frame,
			[output](void *data) { output->on_frame(data); });
	output->request_state.connect(&wlr_output->events.request_state,
			[output](void *data) { output->on_request_state(data); });
	output->destroy.connect(&wlr_output->events.destroy, [output](void *data) {
		/* Frees the layer scene-trees; the wb::Listener members disconnect
		 * themselves when the output is deleted. */
		size_t num_layers =
			sizeof(output->layers) / sizeof(struct wlr_scene_node *);
		for (size_t i = 0; i < num_layers; i++) {
			wlr_scene_node_destroy(
					((struct wlr_scene_node **) &output->layers)[i]);
		}
		wl_list_remove(&output->link);
		delete output;
	});

	/* Adds this to the output layout. The add_auto function arranges outputs
	 * from left-to-right in the order they appear. A more sophisticated
	 * compositor would let the user configure the arrangement of outputs in the
	 * layout.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	if (!l_output) {
		wlr_log(WLR_ERROR, "%s", _("Could not add an output layout."));
		return;
	}

	struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);

	/* Publish the configuration and compute the usable area now that the
	 * output is in the layout. */
	output_manager_update_config(server);
	reconfigure_output(output);
}

void init_output(struct wb_server *server) {
	wl_list_init(&server->outputs);

	server->new_output.connect(&server->backend->events.new_output,
			[server](void *data) { new_output_notify(server, data); });

	server->wlr_output_manager = wlr_output_manager_v1_create(server->wl_display);
	server->output_configuration_applied.connect(&server->wlr_output_manager->events.apply,
			[server](void *data) { output_configuration_applied(server, data); });
	server->output_configuration_tested.connect(&server->wlr_output_manager->events.test,
			[server](void *data) { output_configuration_tested(server, data); });
}
