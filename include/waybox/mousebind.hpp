#ifndef WB_MOUSEBIND_HPP
#define WB_MOUSEBIND_HPP

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "waybox/action.hpp"

namespace wb {

/*
 * Mouse (pointer) binding model — the pointer half of the action framework.
 *
 * rc.xml's <mouse> section binds a button event in a given context (where the
 * pointer is: the desktop Root, a window's Client area, its Titlebar, etc.) to
 * an ordered list of actions, mirroring the keyboard model. The parsing and
 * matching here are pure and unit-tested; the runtime resolves the context and
 * dispatches through run_action() in cursor.cpp.
 */

enum class MouseContext {
	Root,      /* the desktop background (no window under the pointer) */
	Client,    /* a window's content area */
	Titlebar,  /* a server-side titlebar (reachable once SSD lands) */
	Icon,      /* the window icon in the titlebar (<titleLayout> N) */
	Frame,     /* a window's frame/border */
	Desktop,   /* the desktop, distinct from Root in Openbox */
	Top,       /* resize edges */
	Bottom,
	Left,
	Right,
};

enum class MouseEvent {
	Press,
	Release,
	Click,
	DoubleClick,
	Drag,
};

/* Normalized pointer button identifiers. The values match the Linux input
 * BTN_* codes that wlroots reports, so a parsed binding compares directly
 * against a wlr_pointer_button_event without a translation table. */
enum : uint32_t {
	MOUSE_BUTTON_LEFT = 0x110,
	MOUSE_BUTTON_RIGHT = 0x111,
	MOUSE_BUTTON_MIDDLE = 0x112,
	/* The scroll wheel, as Openbox's "Up" and "Down" buttons (outside the
	 * BTN_* range, so they never collide with a real button). */
	MOUSE_BUTTON_UP = 0x7f000001,
	MOUSE_BUTTON_DOWN = 0x7f000002,
};

struct MouseBinding {
	MouseContext context;
	MouseEvent event;
	uint32_t button;     /* a MOUSE_BUTTON_* / BTN_* code */
	uint32_t modifiers;  /* WLR_MODIFIER_* mask */
	std::vector<Action> actions;
};

/* Parse an rc.xml context name ("Root", "Client", ...). nullopt if unknown. */
std::optional<MouseContext> mouse_context_from_name(std::string_view name);

/* Parse an rc.xml mousebind action attribute ("Press", "Click", ...). */
std::optional<MouseEvent> mouse_event_from_name(std::string_view name);

/* A parsed button spec: its modifier mask and button code. */
struct MouseButtonSpec {
	uint32_t modifiers;
	uint32_t button;
};

/* Parse an rc.xml button string, optionally with modifier prefixes, e.g.
 * "Left", "Right", "A-Middle", "C-S-Left". nullopt if the button is unknown. */
std::optional<MouseButtonSpec> parse_mouse_button(std::string_view spec);

/* Whether a binding's context covers where the pointer is: the same
 * context, Frame for anywhere on a window (its client area, titlebar or
 * borders), and Desktop and Root for each other (both are the empty
 * desktop in Waybox). */
bool mouse_context_matches(MouseContext binding, MouseContext actual);

/* Whether a binding fires for the given context/button/modifiers/event.
 * Caps Lock and Num Lock (Mod2) are ignored, as in Openbox. */
bool mouse_binding_matches(const MouseBinding &binding, MouseContext context,
		uint32_t button, uint32_t modifiers, MouseEvent event);

/* Whole scroll-wheel notches in one vertical scroll event, negative for up.
 * `value120` is the event's high-resolution step (120 per notch, 0 if the
 * device gives none) and is added to `*accum`, which carries the fractions
 * of a notch between events. A wheel that reports no steps counts each
 * event as one notch; other sources (touchpads) never make notches. */
int wheel_notches(int32_t *accum, int32_t value120, bool from_wheel, double delta);

/*
 * Turns button presses, motion and releases into Openbox's mouse events:
 * Press on press; Drag once the pointer has moved kDragThreshold pixels with
 * the button held; Release on release, plus Click when it was not a drag,
 * plus DoubleClick when that click follows another click of the same
 * button, on the same target and context, within kDoubleClickMs.
 *
 * `target` identifies the window (or 0 for none); the caller passes
 * forget() a target that goes away. Pure, for unit testing.
 */
class ClickTracker {
public:
	static constexpr double kDragThreshold = 8.0;
	static constexpr uint32_t kDoubleClickMs = 500;

	struct Pressed {
		uint32_t button;
		MouseContext context;
		uintptr_t target;
		uint32_t modifiers;
	};

	std::vector<MouseEvent> press(uint32_t button, MouseContext context,
			uintptr_t target, uint32_t modifiers, double x, double y,
			uint32_t time_ms);
	std::vector<MouseEvent> motion(double x, double y);
	/* Events for releasing `button`; empty if it was not the tracked one. */
	std::vector<MouseEvent> release(uint32_t button, uint32_t time_ms);
	/* The press being tracked (button held), if any. */
	const std::optional<Pressed> &pressed() const { return pressed_; }
	void forget(uintptr_t target);

private:
	std::optional<Pressed> pressed_;
	double press_x_ = 0, press_y_ = 0;
	bool dragged_ = false;
	std::optional<Pressed> last_click_;
	uint32_t last_click_ms_ = 0;
};

}  // namespace wb

#endif /* WB_MOUSEBIND_HPP */
