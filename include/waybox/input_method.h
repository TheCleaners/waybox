#ifndef _WB_INPUT_METHOD_H
#define _WB_INPUT_METHOD_H

#include <stdbool.h>

struct wb_server;
struct wlr_keyboard;
struct wlr_keyboard_key_event;

/*
 * Input methods (fcitx5, ibus, ...): text-input-v3 on the application side,
 * input-method-v2 on the input method's side, and the relay between them.
 * One input method at a time; it gets the keyboard (after Waybox's own
 * bindings) when it grabs it, and its candidate popup is placed at the
 * text cursor.
 */
void init_input_method(struct wb_server *server);
void finish_input_method(struct wb_server *server);

/* Give a key or modifier change to the input method's keyboard grab, if it
 * has one and the event isn't from the input method's own virtual keyboard.
 * True when the input method took it (don't send it to the focused client). */
bool input_method_key(struct wb_server *server, struct wlr_keyboard *keyboard,
		const struct wlr_keyboard_key_event *event);
bool input_method_modifiers(struct wb_server *server, struct wlr_keyboard *keyboard);

#endif /* _WB_INPUT_METHOD_H */
