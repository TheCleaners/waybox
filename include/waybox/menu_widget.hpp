#ifndef WB_MENU_WIDGET_HPP
#define WB_MENU_WIDGET_HPP

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

#include <cairo.h>

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
	/* Open `root_id` placed by ShowMenu's <position> in `area` (layout
	 * coordinates); at (lx, ly), as above, if the position isn't one. */
	struct Placement {
		std::string x, y;
		int area_x = 0, area_y = 0, area_w = 0, area_h = 0;
	};
	bool open(std::string_view root_id, int lx, int ly, const Placement &placement);

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
		int icon_col = 0;  /* this menu's icon column width (0 if no icons) */
		/* Pipe menu id whose output this level is waiting for (it shows the
		 * "Loading…" placeholder until then), or empty. */
		std::string pending_id;
	};

	/* A running pipe-menu command (see start_pipe). */
	struct PipeJob {
		MenuWidget *owner = nullptr;
		std::string id;
		std::string label;
		pid_t pid = -1;
		int fd = -1;
		struct wl_event_source *read_source = nullptr;
		struct wl_event_source *timer = nullptr;
		std::string output;
	};

	int text_width(std::string_view label) const;
	Rect output_bounds(int lx, int ly) const;
	float output_scale(int lx, int ly) const;
	void render_level(Level &level);
	void push_menu(const Menu *menu, const Rect &rect, int parent_item);
	void close_below(size_t keep);  /* close levels deeper than index `keep` */
	void open_submenu(size_t level_index, int item_index);
	int menu_icon_column(const Menu *menu);

	/* Look `id` up in finished pipe-menu output first, then in menu.xml. */
	const Menu *find_menu(std::string_view id) const;
	/* As find_menu, building a script's menu (wb.menu_provider) first. */
	const Menu *resolve_menu(std::string_view id);
	/* `menu`, or a copy whose unlabelled submenu items take their label
	 * from the script providing the menu (kept in labelled_). */
	const Menu *with_labels(const Menu *menu);
	std::deque<Menu> labelled_;
	/* Run a pipe menu's command (unless already running or done); its output
	 * replaces the placeholder via finish_pipe(). */
	void start_pipe(const Menu &menu);
	void finish_pipe(PipeJob *job, bool ok);
	void stop_pipe(PipeJob *job, bool abandon);
	/* Re-open whatever level is showing the placeholder for pipe menu `id`. */
	void refresh_pending(const std::string &id);
	static int on_pipe_readable(int fd, uint32_t mask, void *data);
	static int on_pipe_timeout(void *data);
	cairo_surface_t *icon_surface(const std::string &name);

	struct wb_server *server_;
	const MenuFile &menus_;
	MenuStyle style_;
	MenuBehavior behavior_;
	MenuMetrics metrics_;
	struct wlr_scene_tree *tree_ = nullptr;
	int text_height_ = 0;  /* measured label cell height (logical px) */
	int icon_size_ = 0;    /* icon edge length (logical px) */
	std::string icon_theme_;
	std::map<std::string, cairo_surface_t *> icon_cache_;  /* owned surfaces */
	std::vector<Level> levels_;
	std::vector<Action> pending_;  /* actions of a chosen entry, for the caller */

	/* Pipe menus: finished output (per pipe menu id, kept while the menu is
	 * open; re-run the next time it opens, as Openbox does), running jobs,
	 * and the placeholder shown meanwhile. */
	std::map<std::string, std::unique_ptr<MenuFile>> pipe_results_;
	std::map<std::string, std::unique_ptr<PipeJob>> pipe_jobs_;

	/* Hover-to-open waits behavior_.submenu_delay_ms (<menu hoverDelay>):
	 * the submenu item the pointer rests on, and the timer that opens it. */
	struct wl_event_source *hover_timer_ = nullptr;
	size_t hover_depth_ = 0;
	int hover_item_ = -1;
	void hover_submenu(size_t depth, int item);
	Menu loading_menu_;
	std::string root_id_;
	int root_x_ = 0, root_y_ = 0;
};

}  // namespace wb

#endif /* WB_MENU_WIDGET_HPP */
