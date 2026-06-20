#ifndef WB_STYLE_HPP
#define WB_STYLE_HPP

#include <optional>
#include <string_view>

#include "waybox/menu.hpp"    // MenuMetrics
#include "waybox/theme.hpp"   // Theme (raw themerc model)
#include "waybox/widget.hpp"  // Surface, ControlStyle, TextStyle, ...

namespace wb {

/*
 * Resolved, render-ready presentation + behaviour for each kind of compositor
 * chrome. The renderer consumes these (never wb::Theme directly); they are
 * derived from a Theme by the *_from_theme() adapters but are a superset of
 * themerc, so richer themes/settings can populate fields Openbox never had.
 *
 * Pure data + pure adapters (no Cairo/wlroots); unit-tested in style_test.cpp.
 */

/* ---- Menu (root menu, context menus) -------------------------------- */

struct MenuStyle {
	Surface panel;          /* background, border, padding, opacity */
	Surface title_bar;      /* title strip background */
	TextStyle title_text;
	ControlStyle item;      /* entry fill/fg per state (normal/hover/disabled) */
	TextStyle item_text;    /* font + justify for entries (colour from `item`) */
	Color separator{0x80, 0x80, 0x80, 255};

	/* Geometry (logical px). Layout is computed dynamically from the measured
	 * font size plus these knobs (the "compactness" of the menu). item_height
	 * == 0 => derive it as (measured text height + 2*item_pad_y). */
	int item_height = 0;
	int item_pad_y = 4;    /* vertical padding above/below each label (density) */
	int item_spacing = 0;
	int separator_height = 7;
	int submenu_arrow_width = 16;
	int icon_column_width = 0;
	int min_width = 80;
};

enum class SubmenuOpen { Hover, Click };

struct MenuBehavior {
	SubmenuOpen submenu_open = SubmenuOpen::Hover;
	int submenu_delay_ms = 100;  /* hover dwell before a submenu opens */
	bool wrap = true;            /* keyboard navigation wraps at the ends */
};

/* ---- Window frame (SSD: borders, titlebar, handle, buttons) ---------- */

struct FrameStyle {
	Border border;          /* per-state window border */
	Color client_line{0, 0, 0, 255};  /* frame/client edge colour */
	Surface title_bar;      /* titlebar background */
	TextStyle label;        /* titlebar label */
	Surface handle;         /* bottom resize handle */
	Surface grip;           /* corner grips */
	ControlStyle button;    /* titlebar buttons (iconify/max/close/...) */
	int handle_width = 0;
	int corner_radius = 0;
};

/* ---- Task switcher (Alt+Tab OSD) ------------------------------------- */

enum class SwitcherOrientation { Vertical, Horizontal };

struct SwitcherStyle {
	Surface panel;
	ControlStyle item;
	TextStyle item_text;
	SwitcherOrientation orientation = SwitcherOrientation::Vertical;
	bool show_icons = true;        /* reserved until icon support lands */
	bool show_thumbnails = false;  /* reserved */
};

enum class SwitcherOrder { MRU, Stacking, Spatial };

struct SwitcherBehavior {
	SwitcherOrder order = SwitcherOrder::MRU;
	bool show_on_hold = true;       /* OSD visible while the modifier is held */
	bool current_monitor_only = false;
	bool include_minimized = true;
	bool wrap = true;
};

/* ---- Adapters: raw Theme -> resolved style (pure) -------------------- */

MenuStyle menu_style_from_theme(const Theme &theme);
FrameStyle frame_style_from_theme(const Theme &theme, bool active);
SwitcherStyle switcher_style_from_theme(const Theme &theme);

/* Bridge a resolved MenuStyle to the geometry layout_menu() consumes. */
MenuMetrics menu_metrics(const MenuStyle &style);

/* ---- String -> enum parsers for rc.xml/themerc extension values ----- */

std::optional<SubmenuOpen> submenu_open_from_name(std::string_view name);
std::optional<SwitcherOrder> switcher_order_from_name(std::string_view name);

}  // namespace wb

#endif /* WB_STYLE_HPP */
