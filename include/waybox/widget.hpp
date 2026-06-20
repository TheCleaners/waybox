#ifndef WB_WIDGET_HPP
#define WB_WIDGET_HPP

#include <optional>
#include <string>
#include <variant>
#include <vector>

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

/* ---- Paint: how a region is filled --------------------------------- */

enum class GradientDir { Vertical, Horizontal, Diagonal };

struct SolidPaint {
	Color color{0, 0, 0, 255};
};

struct GradientPaint {
	Color from{0, 0, 0, 255};
	Color to{0, 0, 0, 255};
	GradientDir dir = GradientDir::Vertical;
};

/* A bitmap fill from a richer theme pack. `fallback` is used when image
 * loading is unavailable, so the surface is always drawable. */
struct ImagePaint {
	std::string path;
	Color fallback{0, 0, 0, 255};
};

struct ShaderUniform {
	std::string name;
	float value[4] = {0, 0, 0, 0};
	int count = 1;  /* number of components used (1..4) */
};

/*
 * A GPU fragment-shader fill (e.g. an animated neon border or shader background).
 * `animated` means it needs the animation clock to advance a time uniform each
 * frame. `fallback` is the solid colour drawn when shaders are disabled or the
 * backend cannot run them — effects are ALWAYS opt-in with a safe fallback.
 */
struct ShaderPaint {
	std::string name;  /* shader program id */
	std::vector<ShaderUniform> uniforms;
	bool animated = false;
	Color fallback{0, 0, 0, 255};
};

using Paint = std::variant<SolidPaint, GradientPaint, ImagePaint, ShaderPaint>;

/* A Paint from the raw themerc Texture (solid or gradient). */
Paint paint_from_texture(const Texture &tex);

/* The flat colour of a Paint if it is solid, else nullopt (convenience for
 * callers/tests that only care about plain fills). */
std::optional<Color> solid_color(const Paint &paint);

/* ---- Effects (GPU extras; degrade to nothing without acceleration) -- */

struct Shadow {
	bool enabled = false;
	int sigma = 0;       /* blur radius of the drop shadow */
	int dx = 0;
	int dy = 0;
	Color color{0, 0, 0, 160};
};

struct Effects {
	Shadow shadow;
	int backdrop_blur = 0;  /* blur radius behind a translucent surface */
};

/*
 * What the active render backend can safely do, plus the user's opt-in. The
 * renderer resolves Paints/Effects against this so a missing GPU or a disabled
 * setting can never produce a broken (or crashing) surface — it falls back to a
 * plain solid fill / no effects. Stability over flair, always.
 */
struct RenderCaps {
	bool effects_enabled = false;  /* user opt-in (settings) */
	bool gpu_shaders = false;      /* backend can run custom fragment shaders */
	bool images = true;            /* backend can load image fills */
};

/* Reduce a Paint to one the current backend can draw without failing. */
Paint effective_paint(const Paint &paint, const RenderCaps &caps);

/* Strip effects the backend/settings cannot safely honour. */
Effects effective_effects(const Effects &effects, const RenderCaps &caps);

/* ---- Borders / text / surfaces ------------------------------------- */

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
 * A fillable, bordered, padded box with optional translucency and effects — the
 * base of every drawn surface (menu panel, titlebar strip, OSD, tooltip, button
 * face).
 */
struct Surface {
	Paint fill;
	Border border;
	Insets padding;
	double opacity = 1.0;
	Effects effects;
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
	Paint fill;
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
