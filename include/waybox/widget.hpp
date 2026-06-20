#ifndef WB_WIDGET_HPP
#define WB_WIDGET_HPP

#include "waybox/theme.hpp"  // Color, Texture, FontSpec, Justify

namespace wb {

/*
 * Shared widget-styling primitives.
 *
 * Every piece of compositor-drawn chrome — menus, the task switcher, window
 * frames/titlebars, tooltips, OSDs — is composed from this small vocabulary so
 * the renderer and the theming layer speak one language and new surfaces don't
 * re-invent geometry/colour handling. These are pure data (no Cairo/wlroots) and
 * unit-tested; the resolved per-widget styles live in style.hpp.
 */

/* Per-side spacing (logical px). */
struct Insets {
	int top = 0;
	int right = 0;
	int bottom = 0;
	int left = 0;

	int horizontal() const { return left + right; }
	int vertical() const { return top + bottom; }
};

/* A drawn edge: thickness, colour, and (waybox extension) corner radius. */
struct Border {
	int width = 0;
	Color color{0, 0, 0, 255};
	int radius = 0;
};

/* How a run of text is drawn. */
struct TextStyle {
	FontSpec font;
	Color color{0, 0, 0, 255};
	Justify justify = Justify::Left;
};

/*
 * A fillable, bordered, padded box with optional translucency — the base of
 * every drawn surface (menu panel, titlebar strip, OSD, tooltip, button face).
 */
struct Surface {
	Texture fill;
	Border border;
	Insets padding;
	double opacity = 1.0;
};

/* Interaction/visual state of a control. */
enum class WidgetState {
	Normal,
	Hover,    /* pointer over / keyboard-highlighted (the "active" item) */
	Pressed,
	Disabled,
};

/* The fill + foreground colour for one control state. */
struct StateStyle {
	Texture fill;
	Color fg{0, 0, 0, 255};
};

/*
 * An interactive item styled per state (menu entries, titlebar buttons,
 * switcher tabs). Unset states fall back to Normal via for_state().
 */
struct ControlStyle {
	StateStyle normal;
	StateStyle hover;
	StateStyle pressed;
	StateStyle disabled;

	const StateStyle &for_state(WidgetState state) const;
};

}  // namespace wb

#endif /* WB_WIDGET_HPP */
