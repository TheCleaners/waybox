#ifndef _WB_CONFIG_H
#define _WB_CONFIG_H

#include <vector>

#include "waybox/action.hpp"
#include "waybox/mousebind.hpp"
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
	struct wl_list key_bindings;
	std::vector<wb::MouseBinding> mouse_bindings;
};

struct wb_key_binding {
	xkb_keysym_t sym;
	uint32_t modifiers;
	std::vector<wb::Action> actions;
	struct wl_list link;
};

bool init_config(struct wb_server *server);
void deinit_config(struct wb_config *config);
#endif
