/*
 * The compositor side of stall.hpp: slow scopes are logged; with
 * WAYBOX_STALL_WATCH=1, a timer every 50 ms notices when the event loop
 * woke late (it was blocked), whatever blocked it, and logs it with the
 * longest timed scope since the last tick as the likely culprit. Off by
 * default: it wakes Waybox 20 times a second.
 */
#include "stall_watch.h"

#include <cstdlib>

#include "waybox/stall.hpp"

namespace {

constexpr int kTickMs = 50;
struct wl_event_source *tick = nullptr;
double expected = 0;

int on_tick(void *) {
	double now = wb::monotonic_ms();
	double late = now - expected;
	std::string culprit = wb::Stalls::get().loop_late(late);
	if (late >= wb::Stalls::get().threshold_ms)
		wlr_log(WLR_INFO, "wb-stall loop %.1f ms (longest timed: %s)", late,
				culprit.empty() ? "nothing timed" : culprit.c_str());
	expected = now + kTickMs;
	wl_event_source_timer_update(tick, kTickMs);
	return 0;
}

}  // namespace

void init_stall_watch(struct wb_server *server) {
	wb::Stalls &s = wb::Stalls::get();
	if (const char *ms = getenv("WAYBOX_STALL_MS")) {
		char *end = nullptr;
		double v = strtod(ms, &end);
		if (end != ms && v >= 0)
			s.threshold_ms = v;
	}
	s.on_slow = [](const std::string &name, double ms) {
		wlr_log(WLR_INFO, "wb-stall %s %.1f ms", name.c_str(), ms);
	};
	const char *watch = getenv("WAYBOX_STALL_WATCH");
	if (watch == nullptr || std::string(watch) != "1")
		return;
	tick = wl_event_loop_add_timer(server->wl_event_loop, on_tick, nullptr);
	if (tick == nullptr)
		return;
	expected = wb::monotonic_ms() + kTickMs;
	wl_event_source_timer_update(tick, kTickMs);
	wlr_log(WLR_INFO, "Event loop watchdog on (WAYBOX_STALL_WATCH=1)");
}

void finish_stall_watch(struct wb_server *) {
	if (tick != nullptr)
		wl_event_source_remove(tick);
	tick = nullptr;
	wb::Stalls::get().on_slow = nullptr;
}
