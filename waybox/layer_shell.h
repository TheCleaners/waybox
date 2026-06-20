#ifndef _WB_LAYERS_H
#define _WB_LAYERS_H
#include "waybox/wlroots.hpp"

#include "waybox/listener.hpp"
#include "waybox/output.h"

struct wb_layer_surface {
	struct wb_output *output = nullptr;
	struct wb_server *server = nullptr;

	struct wlr_scene_layer_surface_v1 *scene = nullptr;

	bool mapped = false;

	wb::Listener destroy;
	wb::Listener map;
	wb::Listener unmap;
	wb::Listener surface_commit;
	wb::Listener new_popup;
};

struct wb_layer_popup {
	struct wlr_xdg_popup *wlr_popup = nullptr;
	struct wlr_scene_tree *scene = nullptr;

	wb::Listener commit;
	wb::Listener destroy;
	wb::Listener new_popup;
};

enum wb_scene_descriptor_type {
	WB_SCENE_DESC_NODE,
	WB_SCENE_DESC_LAYER_SHELL,
	WB_SCENE_DESC_LAYER_SHELL_POPUP,
};

struct wb_scene_descriptor {
	enum wb_scene_descriptor_type type;
	void *data;
	wb::Listener destroy;
};

void init_layer_shell(struct wb_server *server);
void arrange_layers(struct wb_output *output);
void assign_scene_descriptor(struct wlr_scene_node *node,
	enum wb_scene_descriptor_type type, void *data);

#endif
