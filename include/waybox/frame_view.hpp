#ifndef WB_FRAME_VIEW_HPP
#define WB_FRAME_VIEW_HPP

#include <memory>
#include <string>
#include <vector>

#include "waybox/wlroots.hpp"

#include "waybox/frame.hpp"
#include "waybox/scene_buffer.h"
#include "waybox/style.hpp"

namespace wb {

/*
 * Server-side decoration view: the wlroots/Cairo glue that draws a window
 * frame (titlebar + label + buttons + borders) into a toplevel's scene
 * container, styled from a resolved FrameStyle. The pure geometry lives in
 * frame.cpp; this owns the scene nodes and re-renders on state changes.
 *
 * The frame is painted as a border-coloured background rect filling the whole
 * frame, with the titlebar Cairo surface on top; the client surface (placed by
 * the caller at the decoration insets) covers the rest, so the background shows
 * through as the borders.
 */
class FrameView {
public:
	FrameView(struct wlr_scene_tree *container, const FrameStyle &active,
			const FrameStyle &inactive, const FrameMetrics &metrics,
			std::vector<FrameButton> buttons);
	~FrameView();

	FrameView(const FrameView &) = delete;
	FrameView &operator=(const FrameView &) = delete;

	const FrameMetrics &metrics() const { return metrics_; }
	const std::vector<FrameButton> &buttons() const { return buttons_; }
	FrameInsets insets() const { return frame_insets(metrics_); }

	/* Resize the frame to wrap a client of (client_w, client_h). Repaints. */
	void set_client_size(int client_w, int client_h);

	void set_active(bool active);
	void set_title(std::string title);
	void set_maximized(bool maximized);
	/* Highlight the hovered titlebar button (index into buttons(), or -1). */
	void set_hovered_button(int index);
	/* Mark a titlebar button as held down (index into buttons(), or -1). */
	void set_pressed_button(int index);

	/* Button rectangles in frame-local coordinates (for input hit-testing). */
	std::vector<Rect> button_rects() const;
	int outer_width() const { return outer_w_; }
	int outer_height() const { return outer_h_; }

private:
	const FrameStyle &style() const { return active_state_ ? active_ : inactive_; }
	void layout();
	void render_titlebar();

	FrameStyle active_;
	FrameStyle inactive_;
	FrameMetrics metrics_;
	std::vector<FrameButton> buttons_;

	struct wlr_scene_rect *grab_ = nullptr;      /* invisible resize-grab area */
	struct wlr_scene_rect *bg_ = nullptr;        /* border-coloured backdrop */
	std::unique_ptr<SceneCanvas> titlebar_;
	int outer_w_ = 0;
	int outer_h_ = 0;
	bool active_state_ = true;
	bool maximized_ = false;
	int hovered_button_ = -1;
	int pressed_button_ = -1;
	std::string title_;
	float scale_ = 1.0f;
};

}  // namespace wb

#endif /* WB_FRAME_VIEW_HPP */
