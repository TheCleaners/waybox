#include "waybox/bell_style.hpp"

#include <algorithm>

namespace wb {

namespace {

std::string_view trim(std::string_view s) {
	while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
		s.remove_prefix(1);
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
		s.remove_suffix(1);
	return s;
}

/* A whole number, else false. */
bool number(std::string_view s, long *out) {
	s = trim(s);
	if (s.empty() || s.size() > 9)
		return false;
	long v = 0;
	for (char c : s) {
		if (c < '0' || c > '9')
			return false;
		v = v * 10 + (c - '0');
	}
	*out = v;
	return true;
}

}  // namespace

BellStyle bell_style_with(const BellStyle &base, const std::map<std::string, std::string> &over,
		std::vector<std::string> *problems) {
	BellStyle s = base;
	auto bad = [problems](const std::string &what, std::string_view v) {
		if (problems != nullptr)
			problems->push_back(what + "=\"" + std::string(v) + "\"");
	};
	auto get = [&over](const char *k) -> std::string_view {
		auto it = over.find(k);
		return it == over.end() ? std::string_view() : trim(it->second);
	};
	bool duration_given = !get("duration").empty();
	if (std::string_view flash = get("flash"); !flash.empty()) {
		BellFlash was = s.flash;
		if (flash == "yes" || flash == "window" || flash == "true" || flash == "on")
			s.flash = BellFlash::Window;
		else if (flash == "border")
			s.flash = BellFlash::Border;
		else if (flash == "screen")
			s.flash = BellFlash::Screen;
		else if (flash == "osd")
			s.flash = BellFlash::Osd;
		else if (flash == "no" || flash == "false" || flash == "off")
			s.flash = BellFlash::None;
		else
			bad("flash", flash);
		/* A note is read, not flashed: longer by default. */
		if (!duration_given && (was == BellFlash::Osd) != (s.flash == BellFlash::Osd))
			s.duration_ms = s.flash == BellFlash::Osd ? 1500 : 150;
	}
	bool osd = s.flash == BellFlash::Osd;
	if (std::string_view color = get("color"); !color.empty()) {
		if (auto c = parse_color(color))
			s.color = *c;
		else
			bad("color", color);
	}
	long v;
	if (std::string_view d = get("duration"); !d.empty()) {
		if (number(d, &v))
			s.duration_ms = static_cast<int>(std::clamp(v, 50L, osd ? 10000L : 2000L));
		else
			bad("duration", d);
	}
	s.duration_ms = std::clamp(s.duration_ms, 50, osd ? 10000 : 2000);
	if (std::string_view c = get("count"); !c.empty()) {
		if (number(c, &v))
			s.count = static_cast<int>(std::clamp(v, 1L, 5L));
		else
			bad("count", c);
	}
	if (std::string_view w = get("width"); !w.empty()) {
		if (number(w, &v))
			s.width = static_cast<int>(std::clamp(v, 1L, 64L));
		else
			bad("width", w);
	}
	if (osd)
		s.count = 1;
	return s;
}

BellStyle parse_bell_style(std::string_view flash, std::string_view color,
		std::string_view duration, std::string_view count, std::string_view width,
		std::vector<std::string> *problems) {
	return bell_style_with(BellStyle{}, {{"flash", std::string(flash)},
			{"color", std::string(color)}, {"duration", std::string(duration)},
			{"count", std::string(count)}, {"width", std::string(width)}}, problems);
}

std::vector<Blink> blink_schedule(const BellStyle &style) {
	std::vector<Blink> out;
	if (style.flash == BellFlash::None)
		return out;
	int d = std::max(1, style.duration_ms);
	/* Off as long as on, and never more than 3 a second. */
	int period = std::max(2 * d, kBellMinPeriodMs);
	int n = style.flash == BellFlash::Osd ? 1 : std::clamp(style.count, 1, 5);
	for (int i = 0; i < n; ++i)
		out.push_back({i * period, i * period + d});
	return out;
}

bool BellPacer::ready(uint32_t now) const {
	if (!any_)
		return true;
	if (static_cast<int32_t>(now - busy_until_) < 0)
		return false;
	return now - last_onset_ >= static_cast<uint32_t>(kBellMinPeriodMs);
}

bool RingGate::pass(uint32_t now) {
	if (any_ && now - last_ < static_cast<uint32_t>(kBellMinPeriodMs))
		return false;
	any_ = true;
	last_ = now;
	return true;
}

bool BellPacer::allow(uint32_t now, const std::vector<Blink> &schedule) {
	if (schedule.empty())
		return false;
	if (any_) {
		/* (Unsigned differences: right across the wrap of the clock.) */
		if (static_cast<int32_t>(now - busy_until_) < 0)
			return false;
		if (now - last_onset_ < static_cast<uint32_t>(kBellMinPeriodMs))
			return false;
	}
	any_ = true;
	last_onset_ = now + static_cast<uint32_t>(schedule.back().on_ms);
	busy_until_ = now + static_cast<uint32_t>(schedule.back().off_ms);
	return true;
}

}  // namespace wb
