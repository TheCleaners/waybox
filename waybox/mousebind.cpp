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
	if (token == "Up")
		return MOUSE_BUTTON_UP;
	if (token == "Down")
		return MOUSE_BUTTON_DOWN;
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
	if (name == "Icon")
		return MouseContext::Icon;
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

bool mouse_context_matches(MouseContext binding, MouseContext actual) {
	if (binding == actual)
		return true;
	auto is_desktop = [](MouseContext c) {
		return c == MouseContext::Root || c == MouseContext::Desktop;
	};
	if (is_desktop(binding))
		return is_desktop(actual);
	if (binding == MouseContext::Frame)
		return !is_desktop(actual);
	return false;
}

bool mouse_binding_matches(const MouseBinding &binding, MouseContext context,
		uint32_t button, uint32_t modifiers, MouseEvent event) {
	constexpr uint32_t kLocks = kCaps | kMod2;
	return mouse_context_matches(binding.context, context) &&
			binding.button == button && binding.event == event &&
			(binding.modifiers & ~kLocks) == (modifiers & ~kLocks);
}

int wheel_notches(int32_t *accum, int32_t value120, bool from_wheel, double delta) {
	if (value120 != 0) {
		if ((*accum < 0) != (value120 < 0))
			*accum = 0;  /* changed direction: drop the old fraction */
		*accum += value120;
		int notches = *accum / 120;
		*accum -= notches * 120;
		return notches;
	}
	if (from_wheel && delta != 0)
		return delta < 0 ? -1 : 1;
	return 0;
}

std::vector<MouseEvent> ClickTracker::press(uint32_t button,
		MouseContext context, uintptr_t target, uint32_t modifiers, double x,
		double y, uint32_t time_ms) {
	(void) time_ms;
	pressed_ = Pressed{button, context, target, modifiers};
	press_x_ = x;
	press_y_ = y;
	dragged_ = false;
	return {MouseEvent::Press};
}

std::vector<MouseEvent> ClickTracker::motion(double x, double y) {
	if (!pressed_ || dragged_)
		return {};
	double dx = x - press_x_, dy = y - press_y_;
	if (dx * dx + dy * dy < kDragThreshold * kDragThreshold)
		return {};
	dragged_ = true;
	last_click_.reset();  /* a drag breaks a double-click */
	return {MouseEvent::Drag};
}

std::vector<MouseEvent> ClickTracker::release(uint32_t button, uint32_t time_ms) {
	if (!pressed_ || pressed_->button != button)
		return {};
	Pressed p = *pressed_;
	pressed_.reset();
	std::vector<MouseEvent> events{MouseEvent::Release};
	if (dragged_)
		return events;
	events.push_back(MouseEvent::Click);
	bool doubled = last_click_ && last_click_->button == p.button &&
			last_click_->target == p.target &&
			last_click_->context == p.context &&
			time_ms - last_click_ms_ <= kDoubleClickMs;
	if (doubled) {
		events.push_back(MouseEvent::DoubleClick);
		last_click_.reset();  /* a third click starts over */
	} else {
		last_click_ = p;
		last_click_ms_ = time_ms;
	}
	return events;
}

void ClickTracker::forget(uintptr_t target) {
	if (pressed_ && pressed_->target == target)
		pressed_.reset();
	if (last_click_ && last_click_->target == target)
		last_click_.reset();
}

}  // namespace wb
