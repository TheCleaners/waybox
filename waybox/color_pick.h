#ifndef WB_COLOR_PICK_H
#define WB_COLOR_PICK_H

#include <cstdint>
#include <functional>
#include <string>

#include "waybox/theme.hpp"

struct wb_server;

/*
 * Picking a color from the screen (IPC "pick_color", wbconf's
 * eyedropper): the pointer becomes a crosshair, and the next left click
 * reads the pixel under it, as shown, instead of reaching the window
 * there. A right click, Escape or 30 seconds cancel. One pick at a time.
 *
 * The pixel is read from a frame of that output rendered for the purpose
 * (and dropped, never shown), with the cursor left out of it.
 */
using ColorPicked = std::function<void(bool ok, const wb::Color &color,
		const std::string &error)>;

bool color_pick_start(struct wb_server *server, ColorPicked done);
bool color_pick_active(struct wb_server *server);
/* Ends a pick with an error ("cancelled", the client went...). */
void color_pick_cancel(struct wb_server *server, const std::string &why);

/* From the pointer and keyboard handlers, while a pick is on: true if
 * the event was the pick's (and must go no further). */
bool color_pick_motion(struct wb_server *server);
bool color_pick_button(struct wb_server *server, uint32_t button, bool pressed);
bool color_pick_key(struct wb_server *server, uint32_t keysym);

void finish_color_pick(struct wb_server *server);

#endif
