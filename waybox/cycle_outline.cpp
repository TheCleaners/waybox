#include "waybox/cycle_outline.hpp"

#include <algorithm>

#include "waybox/frame_view.hpp"
#include "waybox/server.h"
#include "waybox/session_lock.h"
#include "waybox/xdg_shell.h"

namespace wb {

CycleOutline::CycleOutline(struct wb_server *server) : server_(server) {
	tree_ = wlr_scene_tree_create(&server_->scene->tree);
	wlr_scene_node_set_enabled(&tree_->node, false);
}

CycleOutline::~CycleOutline() {
	if (tree_ != nullptr)
		wlr_scene_node_destroy(&tree_->node);
	wlr_log(WLR_INFO, "%s", "wb-cycle-outline hidden");
}

namespace {

/* A ring `w` px wide just inside the box (x, y, bw, bh), in one colour. */
void ring(struct wlr_scene_tree *tree, int x, int y, int bw, int bh, int w,
		const float color[4]) {
	if (bw <= 2 * w || bh <= 2 * w) {  /* too small for a ring: filled */
		struct wlr_scene_rect *r = wlr_scene_rect_create(tree, bw, bh, color);
		wlr_scene_node_set_position(&r->node, x, y);
		return;
	}
	struct {
		int x, y, w, h;
	} sides[] = {
		{x, y, bw, w},                    /* top */
		{x, y + bh - w, bw, w},           /* bottom */
		{x, y + w, w, bh - 2 * w},        /* left */
		{x + bw - w, y + w, w, bh - 2 * w},  /* right */
	};
	for (const auto &s : sides) {
		struct wlr_scene_rect *r = wlr_scene_rect_create(tree, s.w, s.h, color);
		wlr_scene_node_set_position(&r->node, s.x, s.y);
	}
}

}  // namespace

void CycleOutline::show(struct wb_toplevel *t) {
	/* Drawn afresh each time: the window may have moved or changed size. */
	struct wlr_scene_node *child, *tmp;
	wl_list_for_each_safe(child, tmp, &tree_->children, link)
		wlr_scene_node_destroy(child);
	if (t == nullptr || !t->mapped || session_locked(server_)) {
		wlr_scene_node_set_enabled(&tree_->node, false);
		return;
	}
	/* The whole frame: the window's geometry pushed out by the decoration
	 * (none for one that draws its own). */
	struct wlr_box g = t->geometry;
	int x = g.x, y = g.y, w = g.width, h = g.height;
	if (t->frame != nullptr && t->frame->outer_width() > 0) {
		wb::FrameInsets in = t->frame->insets();
		x -= in.left;
		y -= in.top;
		w = t->frame->outer_width();
		h = t->frame->outer_height();
	}
	if (w <= 0 || h <= 0) {
		wlr_scene_node_set_enabled(&tree_->node, false);
		return;
	}
	static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	/* Outside in: 1 px white, 4 px black, 1 px white (kWidth in all). */
	ring(tree_, x, y, w, h, 1, white);
	ring(tree_, x + 1, y + 1, w - 2, h - 2, kWidth - 2, black);
	ring(tree_, x + kWidth - 1, y + kWidth - 1, w - 2 * (kWidth - 1), h - 2 * (kWidth - 1), 1,
			white);
	wlr_scene_node_set_enabled(&tree_->node, true);
	wlr_scene_node_raise_to_top(&tree_->node);
	session_lock_keep_on_top(server_);
	wlr_log(WLR_INFO, "wb-cycle-outline %s %d,%d %dx%d", view_app_id(t) ? view_app_id(t) : "",
			x, y, w, h);
}

}  // namespace wb
