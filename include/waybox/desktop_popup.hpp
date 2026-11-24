#ifndef WB_DESKTOP_POPUP_HPP
#define WB_DESKTOP_POPUP_HPP

#include <memory>
#include <string>

#include "waybox/wlroots.hpp"

#include "waybox/scene_buffer.h"
#include "waybox/style.hpp"

struct wb_server;

namespace wb {

/*
 * The desktop name shown for a moment after switching desktops (Openbox's
 * desktop popup, rc.xml <desktops><popupTime>). Centred on the monitor
 * under the pointer, above windows and panels, in the theme's on-screen
 * display style (as the window switcher). Showing it again while it is up
 * replaces the text and restarts the timer.
 */
class DesktopPopup {
public:
	DesktopPopup(struct wb_server *server, const SwitcherStyle &style);
	~DesktopPopup();

	DesktopPopup(const DesktopPopup &) = delete;
	DesktopPopup &operator=(const DesktopPopup &) = delete;

	void show(const std::string &label, int duration_ms);
	void hide();

private:
	struct wb_server *server_;
	SwitcherStyle style_;
	struct wlr_scene_tree *tree_ = nullptr;
	std::unique_ptr<SceneCanvas> canvas_;
	struct wl_event_source *timer_ = nullptr;
};

}  // namespace wb

#endif /* WB_DESKTOP_POPUP_HPP */
