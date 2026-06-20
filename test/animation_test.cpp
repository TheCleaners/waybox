#include "wb_test.hpp"

#include "waybox/animation.hpp"

using wb::AnimationClock;
using wb::Easing;

static bool near(double a, double b) { return (a - b) < 1e-9 && (b - a) < 1e-9; }

WB_TEST(easing_endpoints_and_clamp) {
	for (Easing e : {Easing::Linear, Easing::EaseInQuad, Easing::EaseOutQuad,
				Easing::EaseInOutQuad}) {
		WB_CHECK(near(wb::ease(e, 0.0), 0.0));
		WB_CHECK(near(wb::ease(e, 1.0), 1.0));
		WB_CHECK(near(wb::ease(e, -5.0), 0.0));  /* clamps below */
		WB_CHECK(near(wb::ease(e, 5.0), 1.0));   /* clamps above */
	}
}

WB_TEST(easing_curves_have_expected_shape) {
	WB_CHECK(near(wb::ease(Easing::Linear, 0.5), 0.5));
	WB_CHECK(wb::ease(Easing::EaseInQuad, 0.5) < 0.5);   /* slow start */
	WB_CHECK(wb::ease(Easing::EaseOutQuad, 0.5) > 0.5);  /* fast start */
	WB_CHECK(near(wb::ease(Easing::EaseInOutQuad, 0.5), 0.5));
}

WB_TEST(anim_progress_clamps_and_handles_zero_duration) {
	WB_CHECK(near(wb::anim_progress(100, 200, AnimationClock{100}), 0.0));
	WB_CHECK(near(wb::anim_progress(100, 200, AnimationClock{200}), 0.5));
	WB_CHECK(near(wb::anim_progress(100, 200, AnimationClock{300}), 1.0));
	WB_CHECK(near(wb::anim_progress(100, 200, AnimationClock{999}), 1.0));  /* past end */
	WB_CHECK(near(wb::anim_progress(100, 200, AnimationClock{50}), 0.0));   /* before start */
	WB_CHECK(near(wb::anim_progress(100, 0, AnimationClock{50}), 1.0));     /* zero duration */
}
