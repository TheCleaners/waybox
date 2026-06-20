#ifndef _WB_CONFIG_H
#define _WB_CONFIG_H

#include <vector>

#include "waybox/action.hpp"
#include "waybox/applications.hpp"
#include "waybox/keychain.hpp"
#include "waybox/menu.hpp"
#include "waybox/mousebind.hpp"
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
	struct {
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
	wb::Theme theme;    /* resolved theme (themerc), styles all drawn chrome */
	wb::MenuBehavior menu_behavior;  /* rc.xml <waybox><menu ...> extensions */
};

bool init_config(struct wb_server *server);
void deinit_config(struct wb_config *config);
#endif
