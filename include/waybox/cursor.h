#ifndef _WB_CURSOR_H
#define _WB_CURSOR_H
#include "waybox/wlroots.hpp"

#include <memory>

#include "waybox/listener.hpp"

struct wb_server;

enum wb_cursor_mode {
	WB_CURSOR_PASSTHROUGH,
	WB_CURSOR_MOVE,
	WB_CURSOR_RESIZE,
};

struct wb_cursor {
	struct wlr_cursor *cursor = nullptr;
	struct wlr_xcursor_manager *xcursor_manager = nullptr;

	struct wb_server *server = nullptr;

	enum wb_cursor_mode cursor_mode = WB_CURSOR_PASSTHROUGH;

	wb::Listener cursor_motion;
	wb::Listener cursor_motion_absolute;
	wb::Listener cursor_button;
	wb::Listener cursor_axis;
	wb::Listener cursor_frame;
	wb::Listener pointer_focus_change;
	wb::Listener request_cursor;
	wb::Listener request_set_shape;

	/* wl_signal handlers (wired to the listeners above as lambdas). */
	void on_motion(void *data);
	void on_motion_absolute(void *data);
	void on_button(void *data);
	void on_axis(void *data);
	void on_frame(void *data);
	void on_pointer_focus_change(void *data);
	void on_request_set_cursor(void *data);
	void on_request_set_shape(void *data);

	~wb_cursor();
};

std::unique_ptr<wb_cursor> wb_cursor_create(struct wb_server *server);
void reset_cursor_mode(struct wb_server *server);

#endif /* cursor.h */
