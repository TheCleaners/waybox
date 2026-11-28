#ifndef WB_STYLE_HPP
#define WB_STYLE_HPP

#include <optional>
#include <string>
#include <vector>
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
	int item_pad_y = 3;    /* vertical padding above/below each label (density) */
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
	bool show_icons = true;      /* draw per-item icons when available */
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

/* Where an on-screen display sits on its monitor. */
enum class OsdPosition { Center, Top, Bottom };

/* The look of every on-screen display (the switcher, the desktop name,
 * the bell's note, wb.osd), from the theme (osd.*, else its menu's) and
 * rc.xml's <waybox><osd>. */
struct SwitcherStyle {
	Surface panel;
	ControlStyle item;
	TextStyle item_text;
	/* The desktop popup's squares (Openbox's pager): the shown desktop's
	 * (osd.hilight.bg), the others' (osd.unhilight.bg, else the panel's),
	 * each bordered in the panel's border colour. */
	Paint pager_on, pager_off;
	SwitcherOrientation orientation = SwitcherOrientation::Vertical;
	OsdPosition position = OsdPosition::Center;
	bool show_thumbnails = false;  /* reserved */
};

/* rc.xml <waybox><osd position= opacity= cornerRadius= bg= text= border=/>:
 * each over the theme's where set. */
struct OsdConfig {
	OsdPosition position = OsdPosition::Center;
	std::optional<int> opacity;        /* percent, 10..100 */
	std::optional<int> corner_radius;  /* px, 0..64 */
	std::optional<Color> bg, text, border;
	bool operator==(const OsdConfig &) const = default;
};

/* From rc.xml's strings ("" = not set); a value that doesn't parse is left
 * unset and named in `problems` ("opacity=x"). */
OsdConfig parse_osd_config(std::string_view position, std::string_view opacity,
		std::string_view corner_radius, std::string_view bg, std::string_view text,
		std::string_view border, std::vector<std::string> *problems = nullptr);
std::optional<OsdPosition> osd_position_from_name(std::string_view name);

/* Its top on a monitor box (y, height) for a display `h` tall: centered,
 * or a tenth of the way in from the top or bottom edge (never off it). */
int osd_y(OsdPosition position, int box_y, int box_h, int h);

enum class SwitcherOrder { MRU, Stacking, Spatial };

struct SwitcherBehavior {
	SwitcherOrder order = SwitcherOrder::MRU;
	bool show_on_hold = true;       /* OSD visible while the modifier is held */
	bool current_monitor_only = false;
	bool include_minimized = true;
	bool wrap = true;
	bool show_icons = true;         /* each window's icon beside its title */
	bool close = true;              /* close from it: the rows' x, the keys */
	/* Its keys, as rc.xml writes keys, besides the switcher's own
	 * modifier held ("none": no key): close the selected window (again
	 * on one still there: end its program), and end its program. */
	std::string close_key = "Delete";
	std::string kill_key = "S-Delete";
};

/* ---- Adapters: raw Theme -> resolved style (pure) -------------------- */

MenuStyle menu_style_from_theme(const Theme &theme);
FrameStyle frame_style_from_theme(const Theme &theme, bool active);
SwitcherStyle switcher_style_from_theme(const Theme &theme);
/* The theme's, with rc.xml's <osd> over it. */
SwitcherStyle osd_style(const Theme &theme, const OsdConfig &config);

/* Bridge a resolved MenuStyle to the geometry layout_menu() consumes. */
MenuMetrics menu_metrics(const MenuStyle &style);

/* ---- String -> enum parsers for rc.xml/themerc extension values ----- */

std::optional<SubmenuOpen> submenu_open_from_name(std::string_view name);
std::optional<SwitcherOrder> switcher_order_from_name(std::string_view name);

}  // namespace wb

#endif /* WB_STYLE_HPP */
