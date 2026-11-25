#include "waybox/monitor_layout.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>

namespace wb {

std::pair<int, int> logical_size(int mode_w, int mode_h, double scale,
		std::string_view transform) {
	if (!(scale > 0) || !std::isfinite(scale))
		scale = 1;
	int w = std::max(1, static_cast<int>(std::lround(mode_w / scale)));
	int h = std::max(1, static_cast<int>(std::lround(mode_h / scale)));
	auto ends = [&](std::string_view e) {
		return transform.size() >= e.size() &&
				transform.substr(transform.size() - e.size()) == e;
	};
	/* "90", "270", "flipped-90", "flipped-270": a quarter turn. */
	if (ends("90") || ends("270"))
		std::swap(w, h);
	return {w, h};
}

bool boxes_overlap(const LayoutBox &a, const LayoutBox &b) {
	return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

bool boxes_touch(const LayoutBox &a, const LayoutBox &b) {
	if (boxes_overlap(a, b))
		return false;
	bool x_meet = a.x + a.w == b.x || b.x + b.w == a.x;
	bool y_meet = a.y + a.h == b.y || b.y + b.h == a.y;
	/* Closed intervals: sharing an end counts (a corner). */
	bool x_span = a.x <= b.x + b.w && b.x <= a.x + a.w;
	bool y_span = a.y <= b.y + b.h && b.y <= a.y + a.h;
	return (x_meet && y_span) || (y_meet && x_span);
}

namespace {

/* The nearest of `targets` to `v`, if within `threshold`. */
int snap_one(int v, const std::vector<int> &targets, int threshold) {
	int best = v, dist = threshold + 1;
	for (int t : targets)
		if (std::abs(t - v) < dist) {
			dist = std::abs(t - v);
			best = t;
		}
	return best;
}

bool free_at(const std::vector<LayoutBox> &boxes, size_t i, int x, int y) {
	LayoutBox m = boxes[i];
	m.x = x;
	m.y = y;
	for (size_t j = 0; j < boxes.size(); ++j)
		if (j != i && boxes_overlap(m, boxes[j]))
			return false;
	return true;
}

bool touches_any(const std::vector<LayoutBox> &boxes, size_t i, int x, int y) {
	LayoutBox m = boxes[i];
	m.x = x;
	m.y = y;
	for (size_t j = 0; j < boxes.size(); ++j)
		if (j != i && boxes_touch(m, boxes[j]))
			return true;
	return false;
}

}  // namespace

std::pair<int, int> snap_position(const std::vector<LayoutBox> &boxes, size_t i,
		int x, int y, int threshold) {
	if (i >= boxes.size())
		return {x, y};
	const LayoutBox &m = boxes[i];
	std::vector<int> xs, ys;  /* where its left (top) edge would put an edge on another's */
	for (size_t j = 0; j < boxes.size(); ++j) {
		if (j == i)
			continue;
		const LayoutBox &o = boxes[j];
		xs.insert(xs.end(), {o.x, o.x + o.w, o.x - m.w, o.x + o.w - m.w});
		ys.insert(ys.end(), {o.y, o.y + o.h, o.y - m.h, o.y + o.h - m.h});
	}
	return {snap_one(x, xs, threshold), snap_one(y, ys, threshold)};
}

std::optional<std::pair<int, int>> place_box(const std::vector<LayoutBox> &boxes,
		size_t i, int x, int y) {
	if (i >= boxes.size())
		return std::nullopt;
	if (boxes.size() == 1)
		return std::pair{x, y};
	if (free_at(boxes, i, x, y) && touches_any(boxes, i, x, y))
		return std::pair{x, y};
	const LayoutBox &m = boxes[i];
	std::optional<std::pair<int, int>> best;
	long long best_d = LLONG_MAX;
	auto consider = [&](int cx, int cy) {
		if (!free_at(boxes, i, cx, cy) || !touches_any(boxes, i, cx, cy))
			return;
		long long dx = cx - x, dy = cy - y;
		long long d = dx * dx + dy * dy;
		if (d < best_d) {
			best_d = d;
			best = std::pair{cx, cy};
		}
	};
	for (size_t j = 0; j < boxes.size(); ++j) {
		if (j == i)
			continue;
		const LayoutBox &o = boxes[j];
		/* Beside it, slid along its edge as near as can be (corners
		 * included); and lined up with its edges, as snapping would. */
		int cy = std::clamp(y, o.y - m.h, o.y + o.h);
		int cx = std::clamp(x, o.x - m.w, o.x + o.w);
		for (int yy : {cy, o.y, o.y + o.h - m.h}) {
			consider(o.x - m.w, yy);
			consider(o.x + o.w, yy);
		}
		for (int xx : {cx, o.x, o.x + o.w - m.w}) {
			consider(xx, o.y - m.h);
			consider(xx, o.y + o.h);
		}
	}
	return best;
}

void normalize_layout(std::vector<LayoutBox> &boxes) {
	if (boxes.empty())
		return;
	int mx = INT_MAX, my = INT_MAX;
	for (const LayoutBox &b : boxes) {
		mx = std::min(mx, b.x);
		my = std::min(my, b.y);
	}
	for (LayoutBox &b : boxes) {
		b.x -= mx;
		b.y -= my;
	}
}

std::optional<std::pair<int, int>> parse_position(std::string_view s) {
	size_t comma = s.find(',');
	if (comma == std::string_view::npos || comma == 0 || comma + 1 >= s.size() ||
			s.size() > 32)
		return std::nullopt;
	auto num = [](std::string_view t, int *out) {
		size_t i = 0;
		bool neg = false;
		if (!t.empty() && (t[0] == '-' || t[0] == '+')) {
			neg = t[0] == '-';
			i = 1;
		}
		if (i >= t.size())
			return false;
		long v = 0;
		for (; i < t.size(); ++i) {
			if (t[i] < '0' || t[i] > '9' || v > 1000000)
				return false;
			v = v * 10 + (t[i] - '0');
		}
		*out = static_cast<int>(neg ? -v : v);
		return true;
	};
	int x, y;
	if (!num(s.substr(0, comma), &x) || !num(s.substr(comma + 1), &y))
		return std::nullopt;
	return std::pair{x, y};
}

}  // namespace wb
