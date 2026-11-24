#ifndef WB_SWITCHER_WIDGET_HPP
#define WB_SWITCHER_WIDGET_HPP

#include <memory>
#include <string>
#include <vector>

#include <cairo.h>

#include "waybox/wlroots.hpp"

#include "waybox/scene_buffer.h"
#include "waybox/style.hpp"
#include "waybox/geometry.hpp"

struct wb_server;

namespace wb {

/*
 * The interactive Alt+Tab task switcher OSD.
 *
 * A SwitcherWidget owns a single Cairo-rendered panel, centred on the output
 * under the cursor, listing the cyclable windows and highlighting the current
 * selection. It is purely presentational: the live window list, the trigger
 * modifier mask and the commit/cancel lifecycle live on wb_server (seat.cpp),
 * which advances the selection while the modifier is held and commits focus on
 * release. The widget grabs the keyboard while it exists so Tab/Shift+Tab,
 * Escape and Return never reach clients.
 */
class SwitcherWidget {
public:
	SwitcherWidget(struct wb_server *server, const SwitcherStyle &style,
			const SwitcherBehavior &behavior);
	~SwitcherWidget();

	SwitcherWidget(const SwitcherWidget &) = delete;
	SwitcherWidget &operator=(const SwitcherWidget &) = delete;

	/* One row: a window's title and its icon (or null). */
	struct Entry {
		std::string label;
		cairo_surface_t *icon = nullptr;
	};

	/* Build the OSD from the windows' entries and place it centred, with
	 * `selected` initially highlighted. The icons are referenced, so they
	 * stay valid for as long as the OSD shows them. */
	void open(std::vector<Entry> entries, int selected);

	void select_next();  /* advance one entry (wraps if behavior.wrap) */
	void select_prev();  /* retreat one entry (wraps if behavior.wrap) */

	int selected() const { return selected_; }
	std::size_t size() const { return entries_.size(); }
	bool empty() const { return entries_.empty(); }

private:
	struct wlr_box output_bounds() const;
	float output_scale() const;
	void render();

	struct wb_server *server_;
	SwitcherStyle style_;
	SwitcherBehavior behavior_;
	struct wlr_scene_tree *tree_ = nullptr;
	std::unique_ptr<SceneCanvas> canvas_;
	std::vector<Entry> entries_;
	int icon_size() const;
	int selected_ = 0;
	int text_height_ = 0;
	Rect rect_{0, 0, 0, 0};
};

}  // namespace wb

#endif /* WB_SWITCHER_WIDGET_HPP */
