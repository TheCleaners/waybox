#ifndef WB_ANIMATION_HPP
#define WB_ANIMATION_HPP

#include <cstdint>

namespace wb {

/*
 * Animation seam.
 *
 * SceneFX (or a future GPU painter) supplies shader effects but no animation
 * timeline — that is driven here. The render backend owns per-output frame
 * scheduling; it advances a monotonic clock and ticks registered animations,
 * which update their parameters (e.g. a shader `time` uniform) and report
 * whether a redraw is needed. The easing/progress maths are pure and
 * unit-tested; the Animation interface is the boundary a backend implements.
 *
 * Animations are opt-in and capability-gated (see wb::RenderCaps): when effects
 * are disabled or unaccelerated, nothing animates and surfaces stay static —
 * never a crash, never a stutter that costs the user their session.
 */

enum class Easing {
	Linear,
	EaseInQuad,
	EaseOutQuad,
	EaseInOutQuad,
};

/* Map a normalised time t (clamped to [0,1]) through an easing curve. */
double ease(Easing easing, double t);

/* A monotonic clock the backend advances once per frame. */
struct AnimationClock {
	uint64_t now_ms = 0;
};

/*
 * Linear progress of an animation that began at start_ms and lasts duration_ms,
 * clamped to [0,1]. A zero/!positive duration is treated as already complete.
 */
double anim_progress(uint64_t start_ms, uint64_t duration_ms,
		const AnimationClock &clock);

/*
 * Something whose visual state evolves over time. Backends own the scheduling
 * and call tick() each frame; a true return means the surface needs redrawing.
 */
class Animation {
public:
	virtual ~Animation() = default;
	virtual bool tick(const AnimationClock &clock) = 0;
};

}  // namespace wb

#endif /* WB_ANIMATION_HPP */
