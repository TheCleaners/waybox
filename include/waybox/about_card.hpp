#ifndef WB_ABOUT_CARD_HPP
#define WB_ABOUT_CARD_HPP

/*
 * The About card (the ShowAbout action; "About Waybox" in the root menu):
 * a panel in the middle of the monitor under the pointer, in the switcher's
 * style, with the Waybox icon, its version and what's running. It stays
 * until a click or a key (seat.cpp, cursor.cpp); a click on the project's
 * address opens it.
 */
#include <memory>

#include <cairo.h>

#include "waybox/client_menu.hpp"
#include "waybox/geometry.hpp"
#include "waybox/scene_buffer.h"
#include "waybox/style.hpp"

struct wb_server;

namespace wb {

class AboutCard {
public:
	AboutCard(struct wb_server *server, const SwitcherStyle &style, AboutInfo info);
	~AboutCard();
	AboutCard(const AboutCard &) = delete;
	AboutCard &operator=(const AboutCard &) = delete;

	/* Whether layout point (x, y) is on the project's address. */
	bool on_link(double x, double y) const;

private:
	void render();

	struct wb_server *server_;
	SwitcherStyle style_;
	AboutInfo info_;
	struct wlr_scene_tree *tree_ = nullptr;
	std::unique_ptr<SceneCanvas> canvas_;
	cairo_surface_t *logo_ = nullptr;
	Rect rect_{0, 0, 0, 0};
	Rect link_{0, 0, 0, 0};  /* the address, in layout coordinates */
};

}  // namespace wb

#endif /* WB_ABOUT_CARD_HPP */
