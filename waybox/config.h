#ifndef _WB_CONFIG_H
#define _WB_CONFIG_H

#include <vector>

#include "waybox/action.hpp"
#include "waybox/applications.hpp"
#include "waybox/desktops.hpp"
#include "waybox/keychain.hpp"
#include "waybox/menu.hpp"
#include "waybox/mousebind.hpp"
#include "waybox/output_rules.hpp"
#include "waybox/placement.hpp"
#include "waybox/style.hpp"
#include "waybox/theme.hpp"
#include "waybox/server.h"

struct wb_config {
	struct wb_server *server;
	struct {
		char *layout;
		char *model;
		char *options;
		char *rules;
		char *variant;

		bool use_config;
	} keyboard_layout;
	struct libinput_config_t {
		char *accel_profile;
		char *accel_speed;
		char *calibration_matrix;
		char *click_method;
		char *dwt;
		char *dwtp;
		char *left_handed;
		char *middle_emulation;
		char *natural_scroll;
		char *scroll_button;
		char *scroll_button_lock;
		char *scroll_method;
		char *tap;
		char *tap_button_map;
		char *tap_drag;
		char *tap_drag_lock;

		bool use_config;
	} libinput_config;
	struct {
		int bottom;
		int left;
		int right;
		int top;
	} margins;

	struct wl_list applications;
	std::vector<wb::KeyBinding> key_bindings;
	std::vector<wb::MouseBinding> mouse_bindings;
	std::vector<wb::AppRule> app_rules;
	wb::PlacementPolicy placement_policy = wb::PlacementPolicy::Smart;
	wb::MenuFile menu;  /* parsed menu.xml (root-menu and submenus) */
	wb::MenuSource menu_source;  /* native widget or external launcher */
	wb::Theme theme;    /* resolved theme (themerc), styles all drawn chrome */
	wb::MenuBehavior menu_behavior;  /* rc.xml <waybox><menu ...> extensions */
	wb::SwitcherBehavior switcher_behavior;  /* rc.xml <waybox><switcher ...> */
	wb::DesktopConfig desktops;  /* rc.xml <desktops> */

	/* rc.xml <waybox><virtualInput allow="unlocked|always|never"/>: when input
	 * from virtual keyboards/pointers (wtype, wlrctl, on-screen keyboards,
	 * remote-desktop tools) is accepted. "unlocked" (default) drops it while
	 * the session is locked, so a client cannot type into the locker;
	 * "always" allows it (e.g. an on-screen keyboard for the lock screen);
	 * "never" ignores virtual devices entirely. */
	enum class VirtualInput { Unlocked, Always, Never };
	VirtualInput virtual_input = VirtualInput::Unlocked;

	/* rc.xml <waybox><xwayland enabled="yes|no"/>: run X11 programs
	 * through Xwayland (started on first use). */
	bool xwayland = true;

	/* rc.xml <waybox><hdr outputs="HDMI-A-1 DP-1"/>: outputs to drive in
	 * HDR10 (BT.2020 primaries, PQ), when the output and renderer can. */
	std::vector<std::string> hdr_outputs;

	/* rc.xml <waybox><outputs><output name=... mode=... scale=...
	 * position=... transform=... enabled=... adaptiveSync=.../></outputs>:
	 * per-output settings, matched by connector name or make/model/serial. */
	std::vector<wb::OutputRule> output_rules;

	/* rc.xml <waybox><edgeTiling enabled="yes|no" range="N"/>: drag a window
	 * to a monitor edge to tile it (halves, corner quarters, maximize at the
	 * top); range is how close to the edge the pointer must be, in pixels. */
	struct {
		bool enabled = true;
		int range = 10;
	} edge_tiling;

	/* rc.xml <waybox><titlebar ...> server-side-decoration tuning. A value of
	 * -1 means "unset" (fall back to a sensible default derived from the
	 * theme/font); see frame_metrics() in decoration.cpp. */
	struct {
		int pad_y = -1;        /* vertical padding per side inside the titlebar */
		int button_size = -1;  /* square button edge in px; -1 = auto (2/3 bar) */
		int resize_grab = -1;  /* invisible resize border width; -1 = default */
	} titlebar;
};

/* Startup: read rc.xml into server->config. A broken user rc.xml is
 * reported and the system default used instead (or built-in defaults). */
bool init_config(struct wb_server *server);
/* Reconfigure: read rc.xml into a new config; nullptr (reported) when it is
 * broken, so the caller keeps the current one. */
struct wb_config *reload_config(struct wb_server *server);
void deinit_config(struct wb_config *config);
#endif
