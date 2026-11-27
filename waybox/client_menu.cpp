/*
 * The window menu's contents. Standard-library only (plus the pure menu
 * and action models), so it links into the unit tests.
 */
#include "waybox/client_menu.hpp"

#include <tuple>

#include <optional>

namespace wb {

namespace {

MenuItem entry(std::string label, std::string icon, ActionType type) {
	MenuItem item;
	item.kind = MenuItem::Kind::Entry;
	item.label = std::move(label);
	item.icon = std::move(icon);
	item.actions.push_back(Action{type});
	return item;
}

MenuItem separator() {
	MenuItem item;
	item.kind = MenuItem::Kind::Separator;
	return item;
}

/* The entry that is the window's current state is marked, as Openbox does
 * in this menu. */
std::string marked(const std::string &label, bool current) {
	return (current ? "• " : "") + label;
}

}  // namespace

MenuFile build_client_menu(const ClientMenuState &s) {
	MenuFile file;

	Menu desktops;
	desktops.id = std::string(kClientMenuDesktopsId);
	desktops.label = "Send to desktop";
	for (int i = 0; i < static_cast<int>(s.desktop_names.size()); i++) {
		MenuItem item = entry(marked(s.desktop_names[i],
				!s.omnipresent && i == s.desktop), "", ActionType::SendToDesktop);
		item.actions.back().to = std::to_string(i + 1);
		item.actions.back().follow = false;
		desktops.items.push_back(std::move(item));
	}
	desktops.items.push_back(separator());
	desktops.items.push_back(entry(marked("All desktops", s.omnipresent), "",
			ActionType::ToggleOmnipresent));

	/* Openbox's Layer submenu. */
	Menu layers;
	layers.id = std::string(kClientMenuLayerId);
	layers.label = "Layer";
	for (const auto &[label, value, dir] : {std::tuple{"Always on top", 1, "top"},
			std::tuple{"Normal", 0, "normal"}, std::tuple{"Always on bottom", -1, "bottom"}}) {
		MenuItem item = entry(marked(label, s.layer == value), "", ActionType::SendToLayer);
		item.actions.back().direction = dir;
		layers.items.push_back(std::move(item));
	}

	Menu menu;
	menu.id = std::string(kClientMenuId);
	menu.label = "Window";
	if (s.desktop_names.size() > 1) {
		MenuItem sub;
		sub.kind = MenuItem::Kind::Submenu;
		sub.label = "Send to desktop";
		sub.submenu_id = desktops.id;
		menu.items.push_back(std::move(sub));
		menu.items.push_back(separator());
	}
	{
		MenuItem sub;
		sub.kind = MenuItem::Kind::Submenu;
		sub.label = "Layer";
		sub.submenu_id = layers.id;
		menu.items.push_back(std::move(sub));
	}
	menu.items.push_back(entry("Iconify", "window-minimize", ActionType::Iconify));
	menu.items.push_back(entry(s.maximized ? "Restore" : "Maximize",
			s.maximized ? "window-restore" : "window-maximize",
			ActionType::ToggleMaximize));
	menu.items.push_back(entry("Move", "transform-move", ActionType::Move));
	menu.items.push_back(entry("Resize", "transform-scale", ActionType::Resize));
	menu.items.push_back(entry(s.shaded ? "Roll down" : "Roll up", "",
			s.shaded ? ActionType::Unshade : ActionType::Shade));
	if (s.can_decorate)
		menu.items.push_back(entry(s.decorated ? "Undecorate" : "Decorate", "",
				ActionType::ToggleDecorations));
	menu.items.push_back(separator());
	menu.items.push_back(entry("Close", "window-close", ActionType::Close));
	/* For a window that doesn't close: its program ended (SIGTERM; again
	 * within 3 s, SIGKILL). */
	menu.items.push_back(entry("End Program", "process-stop", ActionType::Kill));

	file.menus.push_back(std::move(menu));
	file.menus.push_back(std::move(desktops));
	file.menus.push_back(std::move(layers));
	return file;
}

}  // namespace wb
