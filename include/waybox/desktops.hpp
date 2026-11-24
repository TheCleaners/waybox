#ifndef WB_DESKTOPS_HPP
#define WB_DESKTOPS_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb {

/*
 * Virtual desktops (rc.xml <desktops>), the pure half: configuration and
 * resolving an action's <to> against the current state. Desktops are 0-based
 * here and 1-based in rc.xml, as in Openbox. The live switching (showing and
 * hiding windows, focus) is in xdg_shell.cpp.
 */

constexpr int kMaxDesktops = 64;

struct DesktopConfig {
	int number = 4;                  /* Openbox's default */
	std::vector<std::string> names;  /* optional; may be shorter than number */
	/* How long the desktop's name shows after switching, in ms; 0 = never
	 * (Openbox's <popupTime>, same default). */
	int popup_time_ms = 875;
};

/* The desktop a GoToDesktop/SendToDesktop <to> names, given the current and
 * previously visited desktop (both 0-based) and the desktop count. <to> is a
 * 1-based number, "current", "last", or a direction: "next"/"right" and
 * "previous"/"left" (desktops form one row, as in Openbox's default layout;
 * "up"/"down" therefore stay put). Directions wrap around the ends when
 * `wrap` is set. Returns std::nullopt for an unknown or out-of-range <to>. */
std::optional<int> desktop_target(std::string_view to, int current, int last,
		int count, bool wrap);

/* The display name of desktop `index` (0-based): its configured name, or
 * "Desktop N". */
std::string desktop_name(const DesktopConfig &config, int index);

/* <popupTime> in ms: 0 (off) to 10 s; `fallback` when it isn't a number. */
int parse_popup_time(std::string_view text, int fallback);

/* A clamped desktop count from rc.xml's <number> text (1..kMaxDesktops). */
int parse_desktop_count(std::string_view text, int fallback);

}  // namespace wb

#endif /* WB_DESKTOPS_HPP */
