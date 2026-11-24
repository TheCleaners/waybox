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
	/* An output-management client (kanshi, wlr-randr) configured it this
	 * session: Reconfigure then leaves it alone rather than reapply rc.xml. */
	bool client_configured = false;
	struct wlr_box geometry = {};
	struct wlr_box usable_area = {};

	wb::Listener destroy;
	wb::Listener frame;
	wb::Listener request_state;

	struct wl_list link;

	void on_frame(void *data);
	void on_request_state(void *data);
};

void init_output(struct wb_server *server);
/* Set HDR (or SDR) colour state on an output state before committing it. */
/* Apply rc.xml's <outputs> rule for every output that has one and that no
 * output-management client has configured (on Reconfigure). */
void apply_output_rules(struct wb_server *server);
void apply_output_color(struct wb_server *server, struct wlr_output *output,
		struct wlr_output_state *state);

#endif /* output.h */
