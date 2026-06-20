/*
 * Pure parsing/matching for mouse bindings. Depends only on the C++ standard
 * library so it can be linked into the standalone unit test.
 *
 * The modifier bit values below mirror WLR_MODIFIER_* (which in turn follow the
 * standard xkb modifier order). config.cpp static_asserts that they still
 * agree with wlroots, so this translation unit can stay wlroots-free.
 */
#include "waybox/mousebind.hpp"

#include <array>
#include <string>

namespace wb {

namespace {

constexpr uint32_t kShift = 1u << 0;
constexpr uint32_t kCaps = 1u << 1;
constexpr uint32_t kCtrl = 1u << 2;
constexpr uint32_t kAlt = 1u << 3;
constexpr uint32_t kMod2 = 1u << 4;
constexpr uint32_t kMod3 = 1u << 5;
constexpr uint32_t kLogo = 1u << 6;
constexpr uint32_t kMod5 = 1u << 7;

std::optional<uint32_t> modifier_from_token(std::string_view token) {
	if (token == "A" || token == "Alt")
		return kAlt;
	if (token == "C" || token == "Ctrl")
		return kCtrl;
	if (token == "S" || token == "Shift")
		return kShift;
	if (token == "W" || token == "Logo")
		return kLogo;
	if (token == "Caps")
		return kCaps;
	if (token == "Mod2")
		return kMod2;
	if (token == "Mod3")
		return kMod3;
	if (token == "Mod5")
		return kMod5;
	return std::nullopt;
}

std::optional<uint32_t> button_from_token(std::string_view token) {
	if (token == "Left")
		return MOUSE_BUTTON_LEFT;
	if (token == "Right")
		return MOUSE_BUTTON_RIGHT;
	if (token == "Middle")
		return MOUSE_BUTTON_MIDDLE;
	return std::nullopt;
}

}  // namespace

std::optional<MouseContext> mouse_context_from_name(std::string_view name) {
	if (name == "Root")
		return MouseContext::Root;
	if (name == "Client")
		return MouseContext::Client;
	if (name == "Titlebar")
		return MouseContext::Titlebar;
	if (name == "Frame")
		return MouseContext::Frame;
	if (name == "Desktop")
		return MouseContext::Desktop;
	if (name == "Top")
		return MouseContext::Top;
	if (name == "Bottom")
		return MouseContext::Bottom;
	if (name == "Left")
		return MouseContext::Left;
	if (name == "Right")
		return MouseContext::Right;
	return std::nullopt;
}

std::optional<MouseEvent> mouse_event_from_name(std::string_view name) {
	if (name == "Press")
		return MouseEvent::Press;
	if (name == "Release")
		return MouseEvent::Release;
	if (name == "Click")
		return MouseEvent::Click;
	if (name == "DoubleClick")
		return MouseEvent::DoubleClick;
	if (name == "Drag")
		return MouseEvent::Drag;
	return std::nullopt;
}

std::optional<MouseButtonSpec> parse_mouse_button(std::string_view spec) {
	uint32_t modifiers = 0;
	std::optional<uint32_t> button;

	std::size_t start = 0;
	while (start <= spec.size()) {
		std::size_t dash = spec.find('-', start);
		std::string_view token = spec.substr(
				start, dash == std::string_view::npos ? std::string_view::npos
				                                       : dash - start);
		const bool is_last = dash == std::string_view::npos;

		if (is_last) {
			button = button_from_token(token);
			if (!button)
				return std::nullopt;
			break;
		}
		/* A non-final token must be a modifier. */
		std::optional<uint32_t> mod = modifier_from_token(token);
		if (!mod)
			return std::nullopt;
		modifiers |= *mod;
		start = dash + 1;
	}

	if (!button)
		return std::nullopt;
	return MouseButtonSpec{modifiers, *button};
}

bool mouse_binding_matches(const MouseBinding &binding, MouseContext context,
		uint32_t button, uint32_t modifiers, MouseEvent event) {
	return binding.context == context && binding.button == button &&
			binding.modifiers == modifiers && binding.event == event;
}

}  // namespace wb
