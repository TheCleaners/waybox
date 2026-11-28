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
class CycleOutline;

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
		std::string note;  /* after the title, dimmed ("not responding") */
	};

	/* Build the OSD from the windows' entries and place it centred, with
	 * `selected` initially highlighted. The icons are referenced, so they
	 * stay valid for as long as the OSD shows them. */
	void open(std::vector<Entry> entries, int selected);

	/* Row `i` gone (its window closed): the list laid out again, the
	 * selection on the row after it (or the last). */
	void remove_entry(int i);

	/* The pointer at layout (lx, ly): the row under it hovered, and its
	 * close button (x) when over it. False when outside the panel. */
	bool on_motion(double lx, double ly);
	/* What's at layout (lx, ly): a row (-1 none) and whether its x. */
	struct Hit {
		int row = -1;
		bool close = false;
	};
	Hit hit(double lx, double ly) const;
	/* Row `i`'s note (after its title). */
	void set_note(int i, const std::string &note);
	/* Select row `i` (a click on it). */
	void select(int i);

	void select_next();  /* advance one entry (wraps if behavior.wrap) */
	void select_prev();  /* retreat one entry (wraps if behavior.wrap) */

	int selected() const { return selected_; }
	/* Whether windows may be closed from it (<switcher close>). */
	bool can_close() const { return behavior_.close; }
	const SwitcherBehavior &behavior() const { return behavior_; }
	/* The window last asked to close from it: asked again (the key or
	 * its x, the window still there), its program is ended. */
	const void *asked_to_close = nullptr;
	/* Above everything again (after the outline was drawn). */
	void raise();

	/* The outline around the selected window (Openbox's focus cycle
	 * indicator): set by whoever cycles, gone with the switcher. */
	std::unique_ptr<CycleOutline> outline;
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
	int hover_ = -1;            /* the row under the pointer */
	bool hover_close_ = false;  /* and over its x */
	int text_height_ = 0;
	/* Row geometry (panel-local), as render() lays them out. */
	int row_h() const;
	int pad_y() const;
	Rect close_rect(int row) const;
	Rect rect_{0, 0, 0, 0};
};

}  // namespace wb

#endif /* WB_SWITCHER_WIDGET_HPP */
