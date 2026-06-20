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

/* Whether a binding fires for the given context/button/modifiers/event. */
bool mouse_binding_matches(const MouseBinding &binding, MouseContext context,
		uint32_t button, uint32_t modifiers, MouseEvent event);

}  // namespace wb

#endif /* WB_MOUSEBIND_HPP */
