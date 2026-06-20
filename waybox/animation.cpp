/*
 * Pure animation maths (easing + progress). No wlroots/Cairo; unit-tested.
 */
#include "waybox/animation.hpp"

namespace wb {

namespace {

double clamp01(double t) {
	if (t < 0.0)
		return 0.0;
	if (t > 1.0)
		return 1.0;
	return t;
}

}  // namespace

double ease(Easing easing, double t) {
	t = clamp01(t);
	switch (easing) {
	case Easing::EaseInQuad:
		return t * t;
	case Easing::EaseOutQuad:
		return t * (2.0 - t);
	case Easing::EaseInOutQuad:
		return t < 0.5 ? 2.0 * t * t : -1.0 + (4.0 - 2.0 * t) * t;
	case Easing::Linear:
		break;
	}
	return t;
}

double anim_progress(uint64_t start_ms, uint64_t duration_ms,
		const AnimationClock &clock) {
	if (duration_ms == 0)
		return 1.0;
	if (clock.now_ms <= start_ms)
		return 0.0;
	uint64_t elapsed = clock.now_ms - start_ms;
	if (elapsed >= duration_ms)
		return 1.0;
	return static_cast<double>(elapsed) / static_cast<double>(duration_ms);
}

}  // namespace wb
