#ifndef _WB_OUTPUT_H
#define _WB_OUTPUT_H

#include <time.h>

#include "waybox/server.h"
#include "waybox/wlroots.hpp"

#include "waybox/listener.hpp"

struct wb_output {
	struct wlr_output *wlr_output = nullptr;
	struct wb_server *server = nullptr;

	struct {
		struct wlr_scene_tree *shell_background;
		struct wlr_scene_tree *shell_bottom;
		struct wlr_scene_tree *shell_fullscreen;
		struct wlr_scene_tree *shell_overlay;
		struct wlr_scene_tree *shell_top;
	} layers;

	bool gamma_lut_changed = false;
	struct wlr_box geometry = {};
	struct wlr_box usable_area = {};

	wb::Listener destroy;
	wb::Listener frame;
	wb::Listener request_state;

	struct wl_list link;

	void on_frame(void *data);
	void on_request_state(void *data);
};

void handle_gamma_control_set_gamma(struct wl_listener *listener, void *data);
void init_output(struct wb_server *server);

#endif /* output.h */
