#ifndef _WB_POINTER_CONSTRAINTS_H
#define _WB_POINTER_CONSTRAINTS_H

/*
 * Pointer constraints (zwp_pointer_constraints_v1) and relative pointer
 * motion (zwp_relative_pointer_manager_v1): mouse-look in games and 3D
 * programs, which lock the pointer in place (or confine it to a region) and
 * read raw motion instead.
 *
 * A constraint is active only while its surface has both keyboard focus
 * and the pointer, so moving the mouse across a window never traps it, and
 * keyboard bindings (Alt+Tab, desktop switching) always release it.
 */

#include <cstdint>

struct wb_server;
struct wlr_input_device;

void init_pointer_constraints(struct wb_server *server);
void finish_pointer_constraints(struct wb_server *server);

/* Re-evaluate which constraint is active (keyboard or pointer focus
 * changed, a constraint appeared or went away). */
void pointer_constraints_update(struct wb_server *server);

/* Relative pointer motion from a device: sends it as raw motion to the
 * focused client and applies the active constraint. Updates dx and dy to the
 * motion the cursor may make; returns false when it must not move at all
 * (pointer locked). */
bool pointer_constraints_motion(struct wb_server *server, uint32_t time_msec,
		double *dx, double *dy, double unaccel_dx, double unaccel_dy);

/* Whether absolute motion (tablets, touch emulation) must be ignored. */
bool pointer_constraints_locked(struct wb_server *server);

#endif
