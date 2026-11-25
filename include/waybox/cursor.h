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
	/* Scroll-wheel fractions (value120) not yet making up a whole notch. */
	int32_t scroll_value120 = 0;

	/* Implicit grab: a surface pressed with a button keeps the pointer
	 * until the last button is released, wherever the pointer goes (a
	 * selection or screenshot box dragged across a panel). Its origin in
	 * layout coordinates, to send motion relative to it. */
	struct wlr_surface *grab_surface = nullptr;
	double grab_origin_x = 0, grab_origin_y = 0;
	wb::Listener grab_surface_destroy;
	/* Buttons physically held, counted here: wlroots' own count misses the
	 * releases Waybox keeps from clients (over a menu, a frame button). */
	int buttons_down = 0;
	void end_grab() {
		grab_surface = nullptr;
		grab_surface_destroy.disconnect();
	}

	wb::Listener cursor_motion;
	wb::Listener cursor_motion_absolute;
	wb::Listener cursor_button;
	wb::Listener cursor_axis;
	wb::Listener cursor_frame;
	wb::Listener pointer_focus_change;
	wb::Listener request_cursor;
	wb::Listener request_set_shape;
	/* Touchpad gestures, passed on to the window under the pointer
	 * (pointer-gestures: pinch to zoom, swipe back and forward). */
	struct wlr_pointer_gestures_v1 *gestures = nullptr;
	wb::Listener swipe_begin, swipe_update, swipe_end;
	wb::Listener pinch_begin, pinch_update, pinch_end;
	wb::Listener hold_begin, hold_end;
	void init_gestures();

	/* wl_signal handlers (wired to the listeners above as lambdas). */
	void on_motion(void *data);
	void on_motion_absolute(void *data);
	void on_button(void *data);
	void handle_button(struct wlr_pointer_button_event *event);
	void on_axis(void *data);
	void on_frame(void *data);
	void on_pointer_focus_change(void *data);
	void on_request_set_cursor(void *data);
	void on_request_set_shape(void *data);

	~wb_cursor();
};

std::unique_ptr<wb_cursor> wb_cursor_create(struct wb_server *server);
void reset_cursor_mode(struct wb_server *server);
/* Re-pick the surface under the pointer after the scene changed underneath it
 * (e.g. a desktop switch), without the pointer having moved. */
void cursor_refresh_focus(struct wb_server *server);

#endif /* cursor.h */
