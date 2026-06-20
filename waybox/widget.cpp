/*
 * Shared widget-styling primitives (pure helpers).
 */
#include "waybox/widget.hpp"

namespace wb {

const StateStyle &ControlStyle::for_state(WidgetState state) const {
	switch (state) {
	case WidgetState::Hover:
		return hover;
	case WidgetState::Pressed:
		return pressed;
	case WidgetState::Disabled:
		return disabled;
	case WidgetState::Normal:
		break;
	}
	return normal;
}

}  // namespace wb
