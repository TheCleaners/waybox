#ifndef WB_MENU_WIDGET_HPP
#define WB_MENU_WIDGET_HPP

#include <memory>
#include <string_view>
#include <vector>

#include "waybox/wlroots.hpp"

#include "waybox/action.hpp"
#include "waybox/menu.hpp"
#include "waybox/scene_buffer.h"
#include "waybox/style.hpp"

struct wb_server;

namespace wb {

/*
 * The interactive root/context menu.
 *
 * A MenuWidget owns a scene subtree of Cairo-rendered panels (one per open
 * level: the root menu plus any open submenus), themed from a resolved
 * MenuStyle. While it exists it "grabs" input: the compositor routes pointer
 * motion/buttons and the Escape key to it instead of clients (see cursor.cpp /
 * seat.cpp). Selecting an entry closes the menu and runs its actions; clicking
 * outside or pressing Escape dismisses it.
 *
 * The pure geometry (layout, hit-testing, placement) lives in menu.cpp and is
 * unit-tested; this class is the wlroots/Cairo glue, exercised by the headless
 * integration test.
 */
class MenuWidget {
public:
	MenuWidget(struct wb_server *server, const MenuFile &menus,
			const MenuStyle &style, const MenuBehavior &behavior);
	~MenuWidget();

	MenuWidget(const MenuWidget &) = delete;
	MenuWidget &operator=(const MenuWidget &) = delete;

	/* Open `root_id` with its top-left near (lx, ly) in layout coordinates.
	 * Returns false (and opens nothing) if the menu id is unknown/empty. */
	bool open(std::string_view root_id, int lx, int ly);

	/* Input while grabbed. on_button returns true if the event dismissed the
	 * menu (so the caller can drop it). on_key handles Escape. When a dismiss
	 * was caused by selecting an entry, take_actions() yields its actions for
	 * the caller to run AFTER destroying the widget (avoiding reentrancy). */
	void on_motion(double lx, double ly);
	bool on_button(uint32_t button, bool pressed);
	bool on_key(xkb_keysym_t sym);
	std::vector<Action> take_actions() { return std::move(pending_); }

	bool empty() const { return levels_.empty(); }

private:
	struct Level {
		const Menu *menu = nullptr;
		std::unique_ptr<SceneCanvas> canvas;
		MenuLayout layout;
		Rect rect;        /* screen position + size (layout coords) */
		int parent_item = -1;  /* the submenu item in the parent that opened it */
		int hovered = -1;
	};

	int text_width(std::string_view label) const;
	Rect output_bounds(int lx, int ly) const;
	float output_scale(int lx, int ly) const;
	void render_level(Level &level);
	void push_menu(const Menu *menu, const Rect &rect, int parent_item);
	void close_below(size_t keep);  /* close levels deeper than index `keep` */
	void open_submenu(size_t level_index, int item_index);

	struct wb_server *server_;
	const MenuFile &menus_;
	MenuStyle style_;
	MenuBehavior behavior_;
	MenuMetrics metrics_;
	struct wlr_scene_tree *tree_ = nullptr;
	int text_height_ = 0;  /* measured label cell height (logical px) */
	std::vector<Level> levels_;
	std::vector<Action> pending_;  /* actions of a chosen entry, for the caller */
};

}  // namespace wb

#endif /* WB_MENU_WIDGET_HPP */
