#ifndef WB_STALL_HPP
#define WB_STALL_HPP

/*
 * Measuring what blocks the event loop. While Waybox is busy, nothing else
 * happens: input waits and frames are late. StallScope times a piece of
 * work by name; every call is counted, and one taking longer than the
 * threshold (8 ms by default, half a 60 Hz frame; WAYBOX_STALL_MS) is
 * logged as "wb-stall NAME MS". The loop watchdog (stall_watch) catches
 * blocking that no scope covers. waybox-msg get_stalls shows the numbers.
 * Pure C++: no wlroots.
 */
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace wb {

struct StallStat {
	uint64_t count = 0;     /* calls */
	uint64_t slow = 0;      /* at or over the threshold */
	double total_ms = 0;
	double max_ms = 0;
};

class Stalls {
public:
	static Stalls &get();

	double threshold_ms = 8.0;
	/* Called for each slow one (the compositor logs it). */
	std::function<void(const std::string &name, double ms)> on_slow;

	void record(const std::string &name, double ms);
	/* The loop was blocked `ms` past when it should have woken: counted
	 * under "loop", with the longest scope since the last tick as the
	 * likely culprit ("" if none). */
	std::string loop_late(double ms);

	const std::map<std::string, StallStat> &stats() const { return stats_; }
	void reset();

private:
	std::map<std::string, StallStat> stats_;
	std::string since_tick_name_;
	double since_tick_ms_ = 0;
};

double monotonic_ms();

/* Times the enclosing block under `name` (a string literal). */
class StallScope {
public:
	explicit StallScope(const char *name) : name_(name), start_(monotonic_ms()) {}
	~StallScope() { Stalls::get().record(name_, monotonic_ms() - start_); }
	StallScope(const StallScope &) = delete;
	StallScope &operator=(const StallScope &) = delete;

private:
	const char *name_;
	double start_;
};

}  // namespace wb

#endif
