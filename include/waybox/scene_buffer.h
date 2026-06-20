#ifndef WB_SCENE_BUFFER_H
#define WB_SCENE_BUFFER_H

#include <cairo.h>

#include "waybox/wlroots.hpp"

namespace wb {

/*
 * A Cairo-backed scene buffer: the bridge from CPU rasterisation to the
 * GPU-composited scene graph. Construct it under a scene tree, paint into
 * cr() with the wb::paint_* helpers (logical coordinates; the canvas is
 * pre-scaled for HiDPI), then commit() to wrap the pixels as a wlr_buffer and
 * attach them to the scene node. The node is displayed at its logical size and
 * is owned by the canvas: destroying the SceneCanvas removes it from the scene.
 */
class SceneCanvas {
public:
	SceneCanvas(struct wlr_scene_tree *parent, int width, int height,
			float scale);
	~SceneCanvas();

	SceneCanvas(const SceneCanvas &) = delete;
	SceneCanvas &operator=(const SceneCanvas &) = delete;

	cairo_t *cr() { return cr_; }
	int width() const { return width_; }
	int height() const { return height_; }

	/* Rasterise the drawing and attach it to the scene node. */
	void commit();

	void set_position(int x, int y);
	struct wlr_scene_buffer *node() { return node_; }

private:
	struct wlr_scene_buffer *node_ = nullptr;
	cairo_surface_t *surface_ = nullptr;
	cairo_t *cr_ = nullptr;
	int width_ = 0;
	int height_ = 0;
};

}  // namespace wb

#endif /* WB_SCENE_BUFFER_H */
