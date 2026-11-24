/*
 * The input method relay: text-input-v3 (applications) <-> input-method-v2
 * (fcitx5, ibus, ...). See input_method.h.
 */
#include "waybox/input_method.h"

#include <list>
#include <memory>

#include "waybox/geometry.hpp"
#include "waybox/listener.hpp"
#include "waybox/seat.h"
#include "waybox/server.h"

struct wb_input_method_relay;

struct wb_text_input {
	wb_input_method_relay *relay;
	struct wlr_text_input_v3 *input;
	wb::Listener enable, commit, disable, destroy;
};

struct wb_ime_popup {
	wb_input_method_relay *relay;
	struct wlr_input_popup_surface_v2 *popup;
	struct wlr_scene_tree *tree;
	struct wlr_box sent_rect = {};
	bool sent = false;
	wb::Listener map, unmap, commit, destroy;
};

struct wb_input_method_relay {
	struct wb_server *server;
	struct wlr_text_input_manager_v3 *text_input_manager;
	struct wlr_input_method_manager_v2 *input_method_manager;
	std::list<std::unique_ptr<wb_text_input>> text_inputs;
	std::list<std::unique_ptr<wb_ime_popup>> popups;
	/* The text input on the focused surface that is enabled, if any. */
	wb_text_input *active = nullptr;
	struct wlr_input_method_v2 *input_method = nullptr;
	struct wlr_input_method_keyboard_grab_v2 *grab = nullptr;
	struct wlr_scene_tree *popup_tree;
	wb::Listener new_text_input, new_input_method, focus_change;
	wb::Listener im_commit, im_grab, im_new_popup, im_destroy, grab_destroy;
};

static struct wlr_surface *focused_surface(wb_input_method_relay *relay) {
	return relay->server->seat->seat->keyboard_state.focused_surface;
}

static bool same_client(struct wl_resource *a, struct wlr_surface *surface) {
	return surface != nullptr &&
			wl_resource_get_client(a) == wl_resource_get_client(surface->resource);
}

/* Where `surface` is on screen (layout coordinates), found in the scene. */
static bool surface_origin(struct wb_server *server, struct wlr_surface *surface,
		int *x, int *y) {
	struct Search {
		struct wlr_surface *target;
		int x = 0, y = 0;
		bool found = false;
	} search{surface};
	wlr_scene_node_for_each_buffer(&server->scene->tree.node,
			[](struct wlr_scene_buffer *buffer, int sx, int sy, void *data) {
		auto *s = static_cast<Search *>(data);
		struct wlr_scene_surface *scene_surface =
				wlr_scene_surface_try_from_buffer(buffer);
		if (!s->found && scene_surface != nullptr &&
				scene_surface->surface == s->target) {
			s->x = sx;
			s->y = sy;
			s->found = true;
		}
	}, &search);
	*x = search.x;
	*y = search.y;
	return search.found;
}

static void update_popup(wb_ime_popup *p) {
	wb_input_method_relay *relay = p->relay;
	struct wlr_surface *surface = p->popup->surface;
	int ox, oy;
	if (relay->active == nullptr || !surface->mapped ||
			!surface_origin(relay->server, relay->active->input->focused_surface,
					&ox, &oy)) {
		wlr_scene_node_set_enabled(&p->tree->node, false);
		return;
	}
	struct wlr_text_input_v3 *input = relay->active->input;
	wb::Rect cursor{ox, oy, 0, 0};
	if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE) {
		const struct wlr_box &r = input->current.cursor_rectangle;
		cursor = {ox + r.x, oy + r.y, r.width, r.height};
	}
	struct wlr_output *output = wlr_output_layout_output_at(
			relay->server->output_layout, cursor.x, cursor.y);
	if (output == nullptr)
		output = wlr_output_layout_get_center_output(relay->server->output_layout);
	struct wlr_box obox = {};
	if (output != nullptr)
		wlr_output_layout_get_box(relay->server->output_layout, output, &obox);
	wb::Rect placed = wb::place_ime_popup(cursor, surface->current.width,
			surface->current.height, wb::Rect{obox.x, obox.y, obox.width, obox.height});

	wlr_scene_node_set_position(&p->tree->node, placed.x, placed.y);
	wlr_scene_node_set_enabled(&p->tree->node, true);
	wlr_scene_node_raise_to_top(&relay->popup_tree->node);

	/* The cursor, relative to the popup, so it can point at it. Only when it
	 * changes: the popup may redraw (and commit) in response. */
	struct wlr_box rect = {cursor.x - placed.x, cursor.y - placed.y,
			cursor.width, cursor.height};
	if (!p->sent || !wlr_box_equal(&rect, &p->sent_rect)) {
		wlr_input_popup_surface_v2_send_text_input_rectangle(p->popup, &rect);
		p->sent_rect = rect;
		p->sent = true;
	}
}

static void update_popups(wb_input_method_relay *relay) {
	for (auto &p : relay->popups)
		update_popup(p.get());
}

/* Tell the input method about the active text input's state. */
static void send_state(wb_input_method_relay *relay) {
	struct wlr_input_method_v2 *im = relay->input_method;
	if (im == nullptr || relay->active == nullptr)
		return;
	struct wlr_text_input_v3 *input = relay->active->input;
	if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT)
		wlr_input_method_v2_send_surrounding_text(im,
				input->current.surrounding.text ? input->current.surrounding.text : "",
				input->current.surrounding.cursor, input->current.surrounding.anchor);
	wlr_input_method_v2_send_text_change_cause(im, input->current.text_change_cause);
	if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE)
		wlr_input_method_v2_send_content_type(im, input->current.content_type.hint,
				input->current.content_type.purpose);
	wlr_input_method_v2_send_done(im);
}

static wb_text_input *find_active(wb_input_method_relay *relay) {
	struct wlr_surface *focused = focused_surface(relay);
	if (focused == nullptr)
		return nullptr;
	for (auto &t : relay->text_inputs)
		if (t->input->current_enabled && t->input->focused_surface == focused)
			return t.get();
	return nullptr;
}

/* Recompute the active text input; (de)activate the input method on change. */
static void update_active(wb_input_method_relay *relay) {
	wb_text_input *active = find_active(relay);
	if (active == relay->active)
		return;
	struct wlr_input_method_v2 *im = relay->input_method;
	if (relay->active != nullptr && im != nullptr) {
		wlr_input_method_v2_send_deactivate(im);
		wlr_input_method_v2_send_done(im);
	}
	relay->active = active;
	if (active != nullptr && im != nullptr) {
		wlr_input_method_v2_send_activate(im);
		send_state(relay);
	}
	update_popups(relay);
}

/* Text inputs follow keyboard focus: they enter surfaces of their own client. */
static void focus_text_inputs(wb_input_method_relay *relay, struct wlr_surface *surface) {
	for (auto &t : relay->text_inputs) {
		struct wlr_text_input_v3 *input = t->input;
		if (input->focused_surface != nullptr && input->focused_surface != surface)
			wlr_text_input_v3_send_leave(input);
		if (input->focused_surface == nullptr && same_client(input->resource, surface))
			wlr_text_input_v3_send_enter(input, surface);
	}
	update_active(relay);
}

static void new_text_input(wb_input_method_relay *relay, struct wlr_text_input_v3 *input) {
	auto owned = std::make_unique<wb_text_input>();
	wb_text_input *t = owned.get();
	t->relay = relay;
	t->input = input;
	t->enable.connect(&input->events.enable, [t](void *) {
		wb_input_method_relay *relay = t->relay;
		if (relay->active == t && relay->input_method != nullptr) {
			/* Enabled again: a fresh start for the input method. */
			wlr_input_method_v2_send_activate(relay->input_method);
			send_state(relay);
		} else {
			update_active(relay);
		}
	});
	t->commit.connect(&input->events.commit, [t](void *) {
		if (t->relay->active == t) {
			send_state(t->relay);
			update_popups(t->relay);
		} else {
			update_active(t->relay);
		}
	});
	t->disable.connect(&input->events.disable, [t](void *) { update_active(t->relay); });
	t->destroy.connect(&input->events.destroy, [t](void *) {
		wb_input_method_relay *relay = t->relay;
		if (relay->active == t) {
			relay->active = nullptr;
			if (relay->input_method != nullptr) {
				wlr_input_method_v2_send_deactivate(relay->input_method);
				wlr_input_method_v2_send_done(relay->input_method);
			}
			update_popups(relay);
		}
		relay->text_inputs.remove_if([t](const auto &p) { return p.get() == t; });
	});
	relay->text_inputs.push_back(std::move(owned));

	struct wlr_surface *focused = focused_surface(relay);
	if (same_client(input->resource, focused))
		wlr_text_input_v3_send_enter(input, focused);
}

static void new_popup(wb_input_method_relay *relay, struct wlr_input_popup_surface_v2 *popup) {
	auto owned = std::make_unique<wb_ime_popup>();
	wb_ime_popup *p = owned.get();
	p->relay = relay;
	p->popup = popup;
	p->tree = wlr_scene_subsurface_tree_create(relay->popup_tree, popup->surface);
	wlr_scene_node_set_enabled(&p->tree->node, false);
	p->map.connect(&popup->surface->events.map, [p](void *) { update_popup(p); });
	p->unmap.connect(&popup->surface->events.unmap, [p](void *) {
		wlr_scene_node_set_enabled(&p->tree->node, false);
	});
	p->commit.connect(&popup->surface->events.commit, [p](void *) {
		if (p->popup->surface->mapped)
			update_popup(p);
	});
	p->destroy.connect(&popup->events.destroy, [p](void *) {
		wlr_scene_node_destroy(&p->tree->node);
		p->relay->popups.remove_if([p](const auto &q) { return q.get() == p; });
	});
	relay->popups.push_back(std::move(owned));
}

static bool from_input_method(wb_input_method_relay *relay, struct wlr_keyboard *keyboard);

/* The keyboard to hand a new grab: the seat's, unless that is the input
 * method's own virtual keyboard (which it gives the grab's keymap: that would
 * change the grab's keymap, again and again), else any other keyboard. */
static struct wlr_keyboard *grab_keyboard(wb_input_method_relay *relay) {
	struct wb_seat *seat = relay->server->seat.get();
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat->seat);
	if (kb != nullptr && !from_input_method(relay, kb))
		return kb;
	struct wb_keyboard *k;
	wl_list_for_each(k, &seat->keyboards, link)
		if (!from_input_method(relay, k->keyboard))
			return k->keyboard;
	return nullptr;
}

static void drop_grab(wb_input_method_relay *relay) {
	relay->grab = nullptr;
	relay->grab_destroy.disconnect();
	/* The client gets the keyboard back: resend the current modifiers. */
	struct wlr_seat *seat = relay->server->seat->seat;
	if (struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat))
		wlr_seat_keyboard_notify_modifiers(seat, &kb->modifiers);
}

static void new_input_method(wb_input_method_relay *relay, struct wlr_input_method_v2 *im) {
	if (relay->input_method != nullptr) {
		wlr_log(WLR_INFO, "%s", _("An input method is already running; refusing another"));
		wlr_input_method_v2_send_unavailable(im);
		return;
	}
	relay->input_method = im;
	relay->im_commit.connect(&im->events.commit, [relay](void *) {
		wb_text_input *active = relay->active;
		struct wlr_input_method_v2 *im = relay->input_method;
		if (active == nullptr)
			return;
		struct wlr_text_input_v3 *input = active->input;
		if (im->current.preedit.text != nullptr)
			wlr_text_input_v3_send_preedit_string(input, im->current.preedit.text,
					im->current.preedit.cursor_begin, im->current.preedit.cursor_end);
		if (im->current.commit_text != nullptr)
			wlr_text_input_v3_send_commit_string(input, im->current.commit_text);
		if (im->current.delete_.before_length != 0 || im->current.delete_.after_length != 0)
			wlr_text_input_v3_send_delete_surrounding_text(input,
					im->current.delete_.before_length, im->current.delete_.after_length);
		wlr_text_input_v3_send_done(input);
	});
	relay->im_grab.connect(&im->events.grab_keyboard, [relay](void *data) {
		relay->grab = static_cast<struct wlr_input_method_keyboard_grab_v2 *>(data);
		wlr_input_method_keyboard_grab_v2_set_keyboard(relay->grab,
				grab_keyboard(relay));
		relay->grab_destroy.connect(&relay->grab->events.destroy,
				[relay](void *) { drop_grab(relay); });
	});
	relay->im_new_popup.connect(&im->events.new_popup_surface, [relay](void *data) {
		new_popup(relay, static_cast<struct wlr_input_popup_surface_v2 *>(data));
	});
	relay->im_destroy.connect(&im->events.destroy, [relay](void *) {
		if (relay->grab != nullptr)
			drop_grab(relay);
		relay->input_method = nullptr;
		relay->im_commit.disconnect();
		relay->im_grab.disconnect();
		relay->im_new_popup.disconnect();
		relay->im_destroy.disconnect();
	});
	wlr_log(WLR_INFO, "%s", _("Input method connected"));
	if (relay->active != nullptr) {
		wlr_input_method_v2_send_activate(im);
		send_state(relay);
	}
}

void init_input_method(struct wb_server *server) {
	auto *relay = new wb_input_method_relay{};
	server->input_method_relay = relay;
	relay->server = server;
	relay->text_input_manager = wlr_text_input_manager_v3_create(server->wl_display);
	relay->input_method_manager = wlr_input_method_manager_v2_create(server->wl_display);
	relay->popup_tree = wlr_scene_tree_create(&server->scene->tree);
	/* The signals were renamed in wlroots 0.20. */
#if WLR_CHECK_VERSION(0, 20, 0)
	struct wl_signal *new_text_input_signal =
			&relay->text_input_manager->events.new_text_input;
	struct wl_signal *new_input_method_signal =
			&relay->input_method_manager->events.new_input_method;
#else
	struct wl_signal *new_text_input_signal =
			&relay->text_input_manager->events.text_input;
	struct wl_signal *new_input_method_signal =
			&relay->input_method_manager->events.input_method;
#endif
	relay->new_text_input.connect(new_text_input_signal, [relay](void *data) {
		new_text_input(relay, static_cast<struct wlr_text_input_v3 *>(data));
	});
	relay->new_input_method.connect(new_input_method_signal,
			[relay](void *data) {
		new_input_method(relay, static_cast<struct wlr_input_method_v2 *>(data));
	});
	relay->focus_change.connect(&server->seat->seat->keyboard_state.events.focus_change,
			[relay](void *data) {
		auto *event = static_cast<struct wlr_seat_keyboard_focus_change_event *>(data);
		focus_text_inputs(relay, event->new_surface);
	});
}

void finish_input_method(struct wb_server *server) {
	delete server->input_method_relay;
	server->input_method_relay = nullptr;
}

/* The input method's own virtual keyboard: what it types (and the keys it
 * passes on) must not go back to it. */
static bool from_input_method(wb_input_method_relay *relay, struct wlr_keyboard *keyboard) {
	struct wlr_virtual_keyboard_v1 *vk =
			wlr_input_device_get_virtual_keyboard(&keyboard->base);
	return vk != nullptr && wl_resource_get_client(vk->resource) ==
			wl_resource_get_client(relay->input_method->resource);
}

bool input_method_key(struct wb_server *server, struct wlr_keyboard *keyboard,
		const struct wlr_keyboard_key_event *event) {
	wb_input_method_relay *relay = server->input_method_relay;
	if (relay == nullptr || relay->grab == nullptr || from_input_method(relay, keyboard))
		return false;
	wlr_input_method_keyboard_grab_v2_set_keyboard(relay->grab, keyboard);
	wlr_input_method_keyboard_grab_v2_send_key(relay->grab, event->time_msec,
			event->keycode, event->state);
	return true;
}

bool input_method_modifiers(struct wb_server *server, struct wlr_keyboard *keyboard) {
	wb_input_method_relay *relay = server->input_method_relay;
	if (relay == nullptr || relay->grab == nullptr || from_input_method(relay, keyboard))
		return false;
	wlr_input_method_keyboard_grab_v2_set_keyboard(relay->grab, keyboard);
	wlr_input_method_keyboard_grab_v2_send_modifiers(relay->grab, &keyboard->modifiers);
	return true;
}
