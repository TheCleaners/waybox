#ifndef WB_CLIENT_MENU_HPP
#define WB_CLIENT_MENU_HPP

#include <string>
#include <string_view>
#include <vector>

#include "waybox/menu.hpp"

namespace wb {

/*
 * The window menu (Openbox's built-in "client-menu"), opened with
 * ShowMenu client-menu: from the titlebar's right button or Alt+Space by
 * default. Its entries depend on the window, so it is built each time it
 * opens. Pure; the live part is in seat.cpp.
 */

constexpr std::string_view kClientMenuId = "client-menu";
constexpr std::string_view kClientMenuDesktopsId = "client-menu-desktops";
constexpr std::string_view kClientMenuLayerId = "client-menu-layer";

struct ClientMenuState {
	std::vector<std::string> desktop_names;  /* one per desktop */
	int desktop = 0;          /* the window's desktop (0-based) */
	bool omnipresent = false;
	int layer = 0;            /* -1 always below, 0 normal, 1 always on top */
	bool maximized = false;
	bool shaded = false;
	bool decorated = false;
	bool can_decorate = true;  /* false for a client that draws its own */
};

MenuFile build_client_menu(const ClientMenuState &state);

}  // namespace wb

#endif /* WB_CLIENT_MENU_HPP */
