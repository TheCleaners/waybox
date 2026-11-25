#include "waybox/stall.hpp"

#include <ctime>

namespace wb {

Stalls &Stalls::get() {
	static Stalls s;
	return s;
}

double monotonic_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1e6;
}

void Stalls::record(const std::string &name, double ms) {
	StallStat &s = stats_[name];
	++s.count;
	s.total_ms += ms;
	if (ms > s.max_ms)
		s.max_ms = ms;
	if (ms > since_tick_ms_) {
		since_tick_ms_ = ms;
		since_tick_name_ = name;
	}
	if (ms >= threshold_ms) {
		++s.slow;
		if (on_slow)
			on_slow(name, ms);
	}
}

std::string Stalls::loop_late(double ms) {
	std::string culprit = since_tick_name_;
	since_tick_name_.clear();
	since_tick_ms_ = 0;
	if (ms < threshold_ms)
		return culprit;
	StallStat &s = stats_["loop"];
	++s.count;
	++s.slow;
	s.total_ms += ms;
	if (ms > s.max_ms)
		s.max_ms = ms;
	return culprit;
}

void Stalls::reset() {
	stats_.clear();
	since_tick_name_.clear();
	since_tick_ms_ = 0;
}

}  // namespace wb
