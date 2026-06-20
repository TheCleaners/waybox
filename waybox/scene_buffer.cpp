/*
 * Cairo-backed wlr_buffer + SceneCanvas. Bridges CPU-rasterised chrome into the
 * scene graph; the GPU composites it like any other buffer.
 */
#include "waybox/scene_buffer.h"

#include <cstdlib>

#include <drm_fourcc.h>

namespace wb {

namespace {

/* A wlr_buffer whose pixels are an ARGB32 Cairo image surface. */
struct CairoBuffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

void cairo_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	CairoBuffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	cairo_surface_destroy(buffer->surface);
	free(buffer);
}

bool cairo_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	CairoBuffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE)
		return false;  /* read-only: the scene only reads it */
	cairo_surface_flush(buffer->surface);
	*data = cairo_image_surface_get_data(buffer->surface);
	*format = DRM_FORMAT_ARGB8888;
	*stride = static_cast<size_t>(cairo_image_surface_get_stride(buffer->surface));
	return true;
}

void cairo_buffer_end_data_ptr_access(struct wlr_buffer *) {}

const struct wlr_buffer_impl cairo_buffer_impl = {
	.destroy = cairo_buffer_destroy,
	.get_dmabuf = nullptr,
	.get_shm = nullptr,
	.begin_data_ptr_access = cairo_buffer_begin_data_ptr_access,
	.end_data_ptr_access = cairo_buffer_end_data_ptr_access,
};

}  // namespace

SceneCanvas::SceneCanvas(struct wlr_scene_tree *parent, int width, int height,
		float scale)
		: width_(width), height_(height) {
	const int phys_w = static_cast<int>(width * scale + 0.5f);
	const int phys_h = static_cast<int>(height * scale + 0.5f);
	surface_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, phys_w, phys_h);
	cr_ = cairo_create(surface_);
	cairo_scale(cr_, scale, scale);  /* draw in logical coordinates */

	node_ = wlr_scene_buffer_create(parent, nullptr);
}

SceneCanvas::~SceneCanvas() {
	if (cr_ != nullptr)
		cairo_destroy(cr_);
	if (surface_ != nullptr)
		cairo_surface_destroy(surface_);
	/* Own the scene node: destroying the canvas removes it from the scene, so a
	 * closed submenu's panel disappears instead of lingering as a stale,
	 * unresponsive artifact. */
	if (node_ != nullptr)
		wlr_scene_node_destroy(&node_->node);
}

void SceneCanvas::commit() {
	if (cr_ == nullptr || surface_ == nullptr || node_ == nullptr)
		return;
	cairo_surface_flush(surface_);

	auto *buffer = static_cast<CairoBuffer *>(calloc(1, sizeof(CairoBuffer)));
	if (buffer == nullptr)
		return;
	wlr_buffer_init(&buffer->base, &cairo_buffer_impl,
			cairo_image_surface_get_width(surface_),
			cairo_image_surface_get_height(surface_));
	/* The buffer takes its own reference to the surface, so it can outlive this
	 * SceneCanvas (the scene holds the buffer) and so the canvas can be redrawn
	 * and committed again (e.g. to update a menu's hover highlight). */
	buffer->surface = cairo_surface_reference(surface_);

	wlr_scene_buffer_set_buffer(node_, &buffer->base);
	wlr_scene_buffer_set_dest_size(node_, width_, height_);  /* logical size */
	wlr_buffer_drop(&buffer->base);
	/* Keep cr_/surface_ alive so the caller may repaint and commit() again. */
}

void SceneCanvas::set_position(int x, int y) {
	if (node_ != nullptr)
		wlr_scene_node_set_position(&node_->node, x, y);
}

}  // namespace wb
