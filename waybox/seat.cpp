#include <libevdev/libevdev.h>
#include <unistd.h>

#include <optional>
#include <vector>

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
#include "waybox/xdg_shell.h"

static void deiconify_toplevel(struct wb_toplevel *toplevel) {
	if (toplevel->xdg_toplevel->requested.minimized) {
		toplevel->xdg_toplevel->requested.minimized = false;
		wl_signal_emit(&toplevel->xdg_toplevel->events.request_minimize, NULL);
	}
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
		if (order[i]->xdg_toplevel->base->surface == focused) {
			current = i;
			break;
		}
	}

	auto eligible = [&order](std::size_t i) {
		return order[i]->scene_tree && order[i]->scene_tree->node.enabled;
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

/* Run a single parsed action against the live compositor state. This is the
 * "live" half of the action framework (the registry/parsing half lives in
 * action.cpp). Adding a new action means adding a case here plus a row in the
 * action.cpp registry. */
void wb::run_action(const wb::Action &action, struct wb_server *server) {
	switch (action.type) {
	case wb::ActionType::Execute:
		wb_spawn(action.command.c_str());
		break;
	case wb::ActionType::NextWindow:
		cycle_toplevels(server);
		break;
	case wb::ActionType::PreviousWindow:
		cycle_toplevels_reverse(server);
		break;
	case wb::ActionType::Close: {
		struct wb_toplevel *toplevel = first_toplevel(server);
		if (toplevel && toplevel->scene_tree->node.enabled)
			wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
		break;
	}
	case wb::ActionType::ToggleMaximize: {
		struct wb_toplevel *toplevel = first_toplevel(server);
		if (toplevel && toplevel->scene_tree->node.enabled)
			wl_signal_emit(&toplevel->xdg_toplevel->events.request_maximize, NULL);
		break;
	}
	case wb::ActionType::Iconify: {
		struct wb_toplevel *toplevel = first_toplevel(server);
		if (toplevel && toplevel->scene_tree->node.enabled) {
			toplevel->xdg_toplevel->requested.minimized = true;
			wl_signal_emit(&toplevel->xdg_toplevel->events.request_minimize, NULL);
		}
		break;
	}
	case wb::ActionType::Shade: {
		struct wb_toplevel *toplevel = first_toplevel(server);
		if (toplevel && toplevel->scene_tree->node.enabled) {
			struct wlr_box geo_box = toplevel->xdg_toplevel->base->geometry;
			int decoration_height = MAX(geo_box.y - toplevel->geometry.y, TITLEBAR_HEIGHT);

			toplevel->previous_geometry = toplevel->geometry;
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
					toplevel->geometry.width, decoration_height);
		}
		break;
	}
	case wb::ActionType::Unshade: {
		struct wb_toplevel *toplevel = first_toplevel(server);
		if (toplevel && toplevel->previous_geometry.height > 0 &&
				toplevel->previous_geometry.width > 0 &&
				toplevel->scene_tree->node.enabled) {
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
					toplevel->previous_geometry.width,
					toplevel->previous_geometry.height);
		}
		break;
	}
	case wb::ActionType::Raise: {
		struct wb_toplevel *toplevel = first_toplevel(server);
		if (toplevel && toplevel->scene_tree->node.enabled)
			raise_toplevel(toplevel);
		break;
	}
	case wb::ActionType::Lower: {
		struct wb_toplevel *toplevel = first_toplevel(server);
		if (toplevel && toplevel->scene_tree->node.enabled)
			lower_toplevel(toplevel);
		break;
	}
	case wb::ActionType::Reconfigure:
		deinit_config(server->config);
		init_config(server);
		break;
	case wb::ActionType::Exit:
		wl_display_terminate(server->wl_display);
		break;
	}
}

static bool handle_keybinding(struct wb_server *server, xkb_keysym_t sym, uint32_t modifiers) {
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 *
	 * Returns true if the keybinding is handled, false to send it to the
	 * client.
	 */

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

	struct wb_key_binding *key_binding;
	wl_list_for_each(key_binding, &server->config->key_bindings, link) {
		if (sym == key_binding->sym && modifiers == key_binding->modifiers) {
			for (const wb::Action &action : key_binding->actions)
				wb::run_action(action, server);
			return true;
		}
	}
	return false;
}

void wb_keyboard::on_modifiers(void *data) {
	/* Raised when a modifier key (shift, alt, ...) is pressed; forward it.
	 *
	 * A seat can only have one keyboard, but that is a Wayland-protocol
	 * limitation, not a wlroots one: we assign every connected keyboard to the
	 * same seat and wlr_seat swaps the underlying wlr_keyboard transparently. */
	wlr_seat_set_keyboard(server->seat->seat, keyboard);
	wlr_seat_keyboard_notify_modifiers(server->seat->seat,
		&keyboard->modifiers);
}

void wb_keyboard::on_key(void *data) {
	/* Raised when a key is pressed or released. */
	auto *event = static_cast<struct wlr_keyboard_key_event *>(data);
	struct wlr_seat *seat = server->seat->seat;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard);
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			handled = handle_keybinding(server, syms[i], modifiers);
		}
	}

	if (!handled) {
		/* Pass it along to the focused client. */
		wlr_seat_set_keyboard(seat, keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}

	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, seat);
}

static void handle_new_keyboard(struct wb_server *server,
		struct wlr_input_device *device) {
	auto *keyboard = new wb_keyboard{};
	keyboard->server = server;
	keyboard->keyboard = wlr_keyboard_from_input_device(device);

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
		wlr_keyboard_set_keymap(keyboard->keyboard, keymap);
		wlr_keyboard_set_repeat_info(keyboard->keyboard, 25, 600);
	}
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);

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

	wlr_seat_set_keyboard(server->seat->seat, keyboard->keyboard);

	wl_list_insert(&server->seat->keyboards, &keyboard->link);
}

#ifdef HAS_LIBINPUT
static bool libinput_config_get_enabled(char *config) {
	return strcmp(config, "disabled") != 0;
}
#endif

static void handle_new_pointer(struct wb_server *server, struct wlr_input_device *device) {
#ifdef HAS_LIBINPUT
	struct wb_config *config = server->config;
	if (config && wlr_input_device_is_libinput(device) && config->libinput_config.use_config) {
		struct libinput_device *libinput_handle =
			wlr_libinput_get_device_handle(device);

		if (config->libinput_config.accel_profile) {
			enum libinput_config_accel_profile accel_profile =
				LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
			if (strcmp(config->libinput_config.accel_profile, "flat") == 0)
				accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
			else if (strcmp(config->libinput_config.accel_profile, "none") == 0)
				accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_NONE;
			libinput_device_config_accel_set_profile(libinput_handle, accel_profile);
		}
		if (config->libinput_config.accel_speed) {
			double accel_speed = strtod(config->libinput_config.accel_speed, NULL);
			libinput_device_config_accel_set_speed(libinput_handle, accel_speed);
		}
		if (config->libinput_config.calibration_matrix) {
			/* Parse up to six space-separated floats from a copy, so we don't
			 * mutate the stored config string (strtok is destructive) and we
			 * never index past matrix[6] or feed strtod a NULL token. */
			char *copy = strdup(config->libinput_config.calibration_matrix);
			if (copy) {
				float matrix[6] = {};
				unsigned short i = 0;
				char *saveptr = NULL;
				char *token = strtok_r(copy, " ", &saveptr);
				while (token != NULL && i < 6) {
					matrix[i++] = strtof(token, NULL);
					token = strtok_r(NULL, " ", &saveptr);
				}
				if (i == 6)
					libinput_device_config_calibration_set_matrix(libinput_handle, matrix);
				free(copy);
			}
		}
		if (config->libinput_config.click_method) {
			enum libinput_config_click_method click_method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
			if (strcmp(config->libinput_config.click_method, "clickfinger") == 0)
				click_method = LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER;
			else if (strcmp(config->libinput_config.click_method, "none") == 0)
				click_method = LIBINPUT_CONFIG_CLICK_METHOD_NONE;
			libinput_device_config_click_set_method(libinput_handle, click_method);
		}
		if (config->libinput_config.dwt) {
			libinput_device_config_dwt_set_enabled(libinput_handle,
					libinput_config_get_enabled(config->libinput_config.dwt));
		}
		if (config->libinput_config.dwtp) {
			libinput_device_config_dwtp_set_enabled(libinput_handle,
					libinput_config_get_enabled(config->libinput_config.dwtp));
		}
		if (config->libinput_config.left_handed) {
			libinput_device_config_left_handed_set(libinput_handle,
					libinput_config_get_enabled(config->libinput_config.left_handed));
		}
		if (config->libinput_config.middle_emulation) {
			libinput_device_config_middle_emulation_set_enabled(libinput_handle,
					libinput_config_get_enabled(config->libinput_config.middle_emulation));
		}
		if (config->libinput_config.natural_scroll) {
			libinput_device_config_scroll_set_natural_scroll_enabled(libinput_handle,
					libinput_config_get_enabled(config->libinput_config.natural_scroll));
		}
		if (config->libinput_config.scroll_button) {
			int button = libevdev_event_code_from_name(EV_KEY, config->libinput_config.scroll_button);
			if (button != -1) {
				libinput_device_config_scroll_set_button(libinput_handle, button);
			}
		}
		if (config->libinput_config.scroll_button_lock) {
			libinput_device_config_scroll_set_button_lock(libinput_handle,
					libinput_config_get_enabled(config->libinput_config.scroll_button_lock));
		}
		if (config->libinput_config.scroll_method) {
			enum libinput_config_scroll_method scroll_method = LIBINPUT_CONFIG_SCROLL_2FG;
			if (strcmp(config->libinput_config.scroll_method, "edge") == 0)
				scroll_method = LIBINPUT_CONFIG_SCROLL_EDGE;
			else if (strcmp(config->libinput_config.scroll_method, "none") == 0)
				scroll_method = LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
			else if (strcmp(config->libinput_config.scroll_method, "button") == 0)
				scroll_method = LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
			libinput_device_config_scroll_set_method(libinput_handle, scroll_method);
		}
		if (config->libinput_config.tap) {
			libinput_device_config_tap_set_enabled(libinput_handle,
					libinput_config_get_enabled(config->libinput_config.tap));
		}
		if (config->libinput_config.tap_button_map) {
			enum libinput_config_tap_button_map map = LIBINPUT_CONFIG_TAP_MAP_LRM;
			if (strcmp(config->libinput_config.tap_button_map, "lmr") == 0)
				map = LIBINPUT_CONFIG_TAP_MAP_LMR;
			libinput_device_config_tap_set_button_map(libinput_handle, map);
		}
		if (config->libinput_config.tap_drag) {
			libinput_device_config_tap_set_drag_enabled(libinput_handle,
					libinput_config_get_enabled(config->libinput_config.tap_drag));
		};
		if (config->libinput_config.tap_drag_lock) {
			libinput_device_config_tap_set_drag_lock_enabled(libinput_handle,
					libinput_config_get_enabled(config->libinput_config.tap_drag_lock));
		};
	}
#endif

	wlr_cursor_attach_input_device(server->cursor->cursor, device);
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
		default:
			wlr_log(WLR_INFO, "%s: %s", _("Unsupported input device detected"), device->name);
			break;
	}

	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->seat->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat->seat, caps);
}

void seat_focus_surface(struct wb_seat *seat, struct wlr_surface *surface) {
	if (!surface) {
		wlr_seat_keyboard_notify_clear_focus(seat->seat);
		return;
	}

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat->seat);
	if (kb != NULL) {
		wlr_seat_keyboard_notify_enter(seat->seat, surface, kb->keycodes,
			kb->num_keycodes, &kb->modifiers);
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

struct wb_seat *wb_seat_create(struct wb_server *server) {
	auto *seat = new wb_seat{};
	wl_list_init(&seat->keyboards);
	server->new_input.connect(&server->backend->events.new_input,
			[server](void *data) { new_input_notify(server, data); });
	seat->seat = wlr_seat_create(server->wl_display, "seat0");

	wlr_primary_selection_v1_device_manager_create(server->wl_display);
	seat->request_set_primary_selection.connect(
			&seat->seat->events.request_set_primary_selection,
			[seat](void *data) {
		auto *event =
			static_cast<struct wlr_seat_request_set_primary_selection_event *>(data);
		wlr_seat_set_primary_selection(seat->seat, event->source, event->serial);
	});
	seat->request_set_selection.connect(
			&seat->seat->events.request_set_selection, [seat](void *data) {
		auto *event =
			static_cast<struct wlr_seat_request_set_selection_event *>(data);
		wlr_seat_set_selection(seat->seat, event->source, event->serial);
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
}
