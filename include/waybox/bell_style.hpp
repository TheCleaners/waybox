#ifndef WB_BELL_STYLE_HPP
#define WB_BELL_STYLE_HPP

/*
 * What the bell looks like (rc.xml <waybox><bell flash= color= duration=
 * count= width=/>), and how its blinks are paced. Pure; unit-tested.
 *
 * Blinks never start more often than 3 times a second (the WCAG limit for
 * flashing), whatever the settings, and a ring while the bell is still
 * showing, or too soon after its last blink, is let go: a program ringing
 * in a loop can't make the screen strobe.
 */
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "waybox/theme.hpp"

namespace wb {

enum class BellFlash {
	None,    /* no: nothing shown */
	Window,  /* yes, window: the whole window (the screen if none rang) */
	Border,  /* the window's edge, `width` thick (the screen if none rang) */
	Screen,  /* the whole monitor */
	Osd,     /* no flash: a note naming the window, on its monitor */
};

struct BellStyle {
	BellFlash flash = BellFlash::None;
	Color color{255, 255, 255, 128};
	int duration_ms = 150;  /* each blink (Osd: how long the note shows) */
	int count = 1;          /* blinks */
	int width = 4;          /* Border */
	bool operator==(const BellStyle &o) const {
		return flash == o.flash && color.r == o.color.r && color.g == o.color.g &&
				color.b == o.color.b && color.a == o.color.a &&
				duration_ms == o.duration_ms && count == o.count && width == o.width;
	}
};

constexpr int kBellMinPeriodMs = 334;  /* at most 3 blinks a second */

/* From the attributes as written ("" = not set). Out-of-range numbers are
 * clamped; what can't be read is left at its default and described in
 * `problems` (if given). */
BellStyle parse_bell_style(std::string_view flash, std::string_view color,
		std::string_view duration, std::string_view count, std::string_view width,
		std::vector<std::string> *problems = nullptr);

/* `base` with the settings in `over` (keys flash, color, duration, count,
 * width; values as rc.xml writes them) put over it, one by one: a Lua
 * "bell" policy's answer, or a preview. A flash style changed without a
 * duration gets that style's default duration. Same checks as above. */
BellStyle bell_style_with(const BellStyle &base, const std::map<std::string, std::string> &over,
		std::vector<std::string> *problems = nullptr);

/* When each blink starts and ends, in ms from the ring. */
struct Blink {
	int on_ms, off_ms;
};
std::vector<Blink> blink_schedule(const BellStyle &style);

/* Lets a ring show, or not: not while the last one is still showing, nor
 * within kBellMinPeriodMs of its last blink. */
class BellPacer {
public:
	bool allow(uint32_t now_ms, const std::vector<Blink> &schedule);

private:
	bool any_ = false;
	uint32_t last_onset_ = 0;
	uint32_t busy_until_ = 0;
};

}  // namespace wb

#endif
