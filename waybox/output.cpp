#include <drm_fourcc.h>
#include <math.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

#include "waybox/wlroots.hpp"

#include "waybox/output.h"
#include "waybox/output_rules.hpp"
#include "waybox/session_lock.h"

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

/* Add HDR10 (BT.2020 primaries, the PQ transfer function, a 10-bit buffer)
 * to `state` when rc.xml lists this output and both the output and the
 * renderer support it; otherwise ask for the default (SDR). If the HDR
 * state fails the output's test, fall back to SDR rather than leave the
 * output dark. */
void apply_output_color(struct wb_server *server, struct wlr_output *output,
		struct wlr_output_state *state) {
#if WLR_CHECK_VERSION(0, 20, 0)
	bool wanted = false;
	if (server->config != nullptr)
		for (const std::string &name : server->config->hdr_outputs)
			wanted |= name == output->name;
	bool possible = server->renderer->features.output_color_transform &&
			(output->supported_primaries & WLR_COLOR_NAMED_PRIMARIES_BT2020) &&
			(output->supported_transfer_functions & WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ);
	if (wanted && !possible)
		wlr_log(WLR_INFO, "%s: %s", output->name,
				server->renderer->features.output_color_transform
						? _("the output does not support HDR10")
						: _("HDR needs the Vulkan renderer"));
	if (wanted && possible) {
		struct wlr_output_image_description hdr = {};
		hdr.primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020;
		hdr.transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ;
		wlr_output_state_set_render_format(state, DRM_FORMAT_XRGB2101010);
		wlr_output_state_set_image_description(state, &hdr);
		if (wlr_output_test_state(output, state)) {
			wlr_log(WLR_INFO, "%s: %s", output->name, _("HDR on"));
			return;
		}
		wlr_log(WLR_ERROR, "%s: %s", output->name, _("HDR refused by the output; using SDR"));
	}
	/* Leave the output alone unless it is in HDR now and shouldn't be. */
	if (output->image_description != nullptr) {
		wlr_output_state_set_render_format(state, DRM_FORMAT_XRGB8888);
		wlr_output_state_set_image_description(state, nullptr);
	}
#else
	(void) server;
	(void) output;
	(void) state;
#endif
}

/* Recompute derived state after an output's configuration changes: refresh the
 * cached layout geometry, re-arrange layer surfaces (which updates the usable
 * area) and reflow toplevels so maximized/fullscreen windows track the new
 * size and floating windows stay reachable. */
/* Put `wlr_output` in the layout (at x,y, or automatically) and make sure
 * it has a scene output linked to its layout entry: one leaving the layout
 * loses that link, so it is made again when it comes back. */
static bool place_output(struct wb_server *server, struct wlr_output *wlr_output,
		std::optional<int> x, std::optional<int> y) {
	bool was_in_layout = wlr_output_layout_get(server->output_layout, wlr_output) != nullptr;
	struct wlr_output_layout_output *l_output = (x && y)
		? wlr_output_layout_add(server->output_layout, wlr_output, *x, *y)
		: (was_in_layout ? wlr_output_layout_get(server->output_layout, wlr_output)
				: wlr_output_layout_add_auto(server->output_layout, wlr_output));
	if (l_output == nullptr) {
		wlr_log(WLR_ERROR, "%s", _("Could not add an output layout."));
		return false;
	}
	if (!was_in_layout) {
		struct wlr_scene_output *scene_output =
				wlr_scene_get_scene_output(server->scene, wlr_output);
		if (scene_output == nullptr)
			scene_output = wlr_scene_output_create(server->scene, wlr_output);
		wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);
	}
	return true;
}

static wb::OutputIdentity output_identity(struct wlr_output *o) {
	return {o->name ? o->name : "", o->make ? o->make : "", o->model ? o->model : "",
			o->serial ? o->serial : ""};
}

static const wb::OutputRule *output_rule(struct wb_server *server, struct wlr_output *o) {
	if (server->config == nullptr)
		return nullptr;
	return wb::match_output(server->config->output_rules, output_identity(o));
}

/* Set up `wlr_output` from its rc.xml rule, or Waybox's defaults (the
 * preferred resolution at its highest refresh rate, a scale for its pixel
 * density). The state is tested first; a mode the output refuses falls back
 * to its preferred mode, so a bad rule can't leave it dark. Returns whether
 * the output is enabled (false too when it couldn't be set up). */
static bool configure_output(struct wb_server *server, struct wlr_output *wlr_output,
		const wb::OutputRule *rule) {
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (rule != nullptr && rule->enabled == false) {
		wlr_output_state_set_enabled(&state, false);
		wlr_output_commit_state(wlr_output, &state);
		wlr_output_state_finish(&state);
		wlr_log(WLR_INFO, "%s: %s", wlr_output->name, _("disabled in rc.xml"));
		return false;
	}
	wlr_output_state_set_enabled(&state, true);

	std::vector<struct wlr_output_mode *> modes;
	std::vector<wb::ModeInfo> infos;
	struct wlr_output_mode *m;
	wl_list_for_each(m, &wlr_output->modes, link) {
		modes.push_back(m);
		infos.push_back({m->width, m->height, m->refresh, m->preferred});
	}
	struct wlr_output_mode *mode = nullptr;
	if (rule != nullptr && rule->width > 0) {
		int i = wb::pick_mode(infos, rule->width, rule->height, rule->refresh);
		if (i >= 0) {
			mode = modes[i];
			if (rule->refresh > 0 && std::fabs(mode->refresh / 1000.0 - rule->refresh) > 0.5)
				wlr_log(WLR_INFO, "%s: %s %dx%d@%.3f", wlr_output->name,
						_("no mode at the refresh rate asked for; using"),
						mode->width, mode->height, mode->refresh / 1000.0);
		} else if (modes.empty()) {
			/* Nested and headless outputs take any size. */
			wlr_output_state_set_custom_mode(&state, rule->width, rule->height,
					(int32_t) (rule->refresh * 1000));
		} else {
			wlr_log(WLR_ERROR, "%s: %s %dx%d", wlr_output->name,
					_("has no mode"), rule->width, rule->height);
		}
	}
	if (mode == nullptr && !modes.empty()) {
		int i = wb::default_mode(infos);
		mode = i >= 0 ? modes[i] : nullptr;
	}
	if (mode != nullptr)
		wlr_output_state_set_mode(&state, mode);

	double scale = (rule != nullptr && rule->scale) ? *rule->scale
			: compute_default_scale(wlr_output, mode);
	wlr_output_state_set_scale(&state, (float) scale);
	if (rule != nullptr && rule->transform)
		wlr_output_state_set_transform(&state,
				static_cast<enum wl_output_transform>(*rule->transform));
	if (rule != nullptr && rule->adaptive_sync)
		wlr_output_state_set_adaptive_sync_enabled(&state, *rule->adaptive_sync);
	apply_output_color(server, wlr_output, &state);

	if (!wlr_output_test_state(wlr_output, &state)) {
		struct wlr_output_mode *preferred = wlr_output_preferred_mode(wlr_output);
		if (preferred != nullptr && preferred != mode) {
			wlr_log(WLR_ERROR, "%s: %s", wlr_output->name,
					_("the output refused that mode; using its preferred mode"));
			wlr_output_state_set_mode(&state, preferred);
			mode = preferred;
		}
	}
	bool committed = wlr_output_commit_state(wlr_output, &state);
	if (!committed)
		wlr_log(WLR_ERROR, "%s: %s", wlr_output->name, _("could not set up the output"));
	else if (mode != nullptr)
		wlr_log(WLR_INFO, "%s: %dx%d@%.3f %s %.2f", wlr_output->name, mode->width,
				mode->height, mode->refresh / 1000.0, _("scale"), scale);
	wlr_output_state_finish(&state);
	/* Not on (it refused even its preferred mode): not in the layout. */
	return committed && wlr_output->enabled;
}

static void reconfigure_output(struct wb_output *output) {
	if (output == NULL) {
		return;
	}
	wlr_output_layout_get_box(output->server->output_layout,
			output->wlr_output, &output->geometry);
	arrange_layers(output);
	arrange_toplevels(output->server);
	session_lock_arrange(output->server);
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

/* Whether a client's configuration `head` changes its output. Clients such
 * as wlr-randr send every output when they change one. */
static bool head_changes_output(struct wb_server *server,
		const struct wlr_output_head_v1_state &head) {
	struct wlr_output *o = head.output;
	if (head.enabled != o->enabled)
		return true;
	if (!head.enabled)
		return false;
	bool mode_differs = head.mode != nullptr ? head.mode != o->current_mode
		: (head.custom_mode.width != o->width || head.custom_mode.height != o->height ||
				(head.custom_mode.refresh != 0 && head.custom_mode.refresh != o->refresh));
	struct wlr_box box = {};
	wlr_output_layout_get_box(server->output_layout, o, &box);
	return mode_differs || head.scale != o->scale || head.transform != o->transform ||
			head.adaptive_sync_enabled != (o->adaptive_sync_status ==
					WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED) ||
			wlr_box_empty(&box) || head.x != box.x || head.y != box.y;
}

/* Apply (or test) an output configuration requested through the
 * output-management protocol. */
static bool apply_output_config(struct wb_server *server,
		struct wlr_output_configuration_v1 *config, bool test_only) {
	bool ok = true;
	struct wlr_output_configuration_head_v1 *head;
	/* Which outputs this changes, judged before anything is committed. */
	std::vector<struct wlr_output *> changed;
	if (!test_only)
		wl_list_for_each(head, &config->heads, link)
			if (head_changes_output(server, head->state))
				changed.push_back(head->state.output);
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
			apply_output_color(server, wlr_output, &state);
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
	/* Clients get the new configuration (positions) back. */

	wl_list_for_each(head, &config->heads, link) {
		struct wlr_output *wlr_output = head->state.output;
		auto *output = static_cast<wb_output *>(wlr_output->data);
		if (output != nullptr && std::find(changed.begin(), changed.end(), wlr_output) !=
				changed.end())
			output->client_configured = true;
		if (head->state.enabled) {
			place_output(server, wlr_output, head->state.x, head->state.y);
			reconfigure_output(output);
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
	if (scene_output == nullptr)
		return;  /* not in the layout */

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
	if (wlr_scene_output_commit(scene_output, NULL))
		session_lock_output_frame(output);

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

	const wb::OutputRule *rule = output_rule(server, wlr_output);
	bool enabled = configure_output(server, wlr_output, rule);

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

	/* Stack this output's layer trees around the shared toplevel container so
	 * panels render correctly: background/bottom below ordinary windows; the
	 * fullscreen, top and overlay layers (where panels like Waybar live) above
	 * them. New toplevels are children of toplevel_tree, so they never lift
	 * above the top/overlay layers. */
	struct wlr_scene_node *tl = &server->toplevel_tree->node;
	wlr_scene_node_place_below(&output->layers.shell_background->node, tl);
	wlr_scene_node_place_below(&output->layers.shell_bottom->node, tl);
	wlr_scene_node_place_above(&output->layers.shell_top->node, tl);
	wlr_scene_node_place_above(&output->layers.shell_fullscreen->node,
			&output->layers.shell_top->node);
	wlr_scene_node_place_above(&output->layers.shell_overlay->node,
			&output->layers.shell_fullscreen->node);

	wl_list_insert(&server->outputs, &output->link);
	/* The new output's layer trees were created on top of the scene. */
	session_lock_keep_on_top(server);

	/* Initialise the usable area to the full output; arrange_layers() will
	 * shrink it once layer-shell clients (panels) reserve exclusive zones. */
	wlr_output_effective_resolution(wlr_output,
			&output->usable_area.width, &output->usable_area.height);

	output->frame.connect(&wlr_output->events.frame,
			[output](void *data) { output->on_frame(data); });
	output->request_state.connect(&wlr_output->events.request_state,
			[output](void *data) { output->on_request_state(data); });
	output->destroy.connect(&wlr_output->events.destroy, [output](void *data) {
		/* Close this output's layer surfaces (panels, wallpapers) first, then
		 * free the layer scene-trees; the wb::Listener members disconnect
		 * themselves when the output is deleted. */
		layer_shell_output_destroy(output);
		size_t num_layers =
			sizeof(output->layers) / sizeof(struct wlr_scene_node *);
		for (size_t i = 0; i < num_layers; i++) {
			wlr_scene_node_destroy(
					((struct wlr_scene_node **) &output->layers)[i]);
		}
		wl_list_remove(&output->link);
		struct wb_server *server = output->server;
		delete output;
		session_lock_arrange(server);
	});

	/* Add it to the output layout, where rc.xml puts it or else to the right
	 * of the others. The layout adds a wl_output global for clients. A
	 * disabled output joins the layout when a client enables it. */
	if (enabled && !place_output(server, wlr_output,
			rule ? rule->x : std::nullopt, rule ? rule->y : std::nullopt))
		return;

	/* Publish the configuration and compute the usable area now that the
	 * output is in the layout. */
	output_manager_update_config(server);
	if (enabled)
		reconfigure_output(output);
}

void apply_output_rules(struct wb_server *server) {
	struct wb_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		const wb::OutputRule *rule = output_rule(server, output->wlr_output);
		if (rule == nullptr || output->client_configured)
			continue;
		if (configure_output(server, output->wlr_output, rule)) {
			if (place_output(server, output->wlr_output, rule->x, rule->y))
				reconfigure_output(output);
		} else {
			wlr_output_layout_remove(server->output_layout, output->wlr_output);
		}
	}
	output_manager_update_config(server);
}

void init_output(struct wb_server *server) {
	wl_list_init(&server->outputs);

	server->new_output.connect(&server->backend->events.new_output,
			[server](void *data) { new_output_notify(server, data); });

	/* Outputs added with add_auto, moved by an output-management client, or
	 * removed change other outputs' positions too: keep every output's layer
	 * trees (and the usable areas windows are placed in) in step. */
	server->output_layout_change.connect(&server->output_layout->events.change,
			[server](void *) {
		struct wb_output *output;
		wl_list_for_each(output, &server->outputs, link)
			arrange_layers(output);
	});

	server->wlr_output_manager = wlr_output_manager_v1_create(server->wl_display);
	server->output_configuration_applied.connect(&server->wlr_output_manager->events.apply,
			[server](void *data) { output_configuration_applied(server, data); });
	server->output_configuration_tested.connect(&server->wlr_output_manager->events.test,
			[server](void *data) { output_configuration_tested(server, data); });
}
