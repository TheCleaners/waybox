#include "waybox/geometry_actions.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace wb {

namespace {

std::string arg(const ActionArgs &args, const char *key) {
	auto it = args.find(key);
	return it == args.end() ? std::string() : it->second;
}

std::optional<int> integer(const std::string &s) {
	if (s.empty())
		return std::nullopt;
	char *end = nullptr;
	long v = std::strtol(s.c_str(), &end, 10);
	if (end == s.c_str() || *end != '\0')
		return std::nullopt;
	return static_cast<int>(std::clamp(v, -100000L, 100000L));
}

/* "N%" of `extent`, or nullopt. */
std::optional<int> percent(const std::string &s, int extent) {
	if (s.size() < 2 || s.back() != '%')
		return std::nullopt;
	auto n = integer(s.substr(0, s.size() - 1));
	if (!n)
		return std::nullopt;
	return extent * *n / 100;
}

/* A position along one axis: pixels from the area's start (negative: from
 * its end), center, N%, current. */
int position(const std::string &s, int area_start, int area_len, int len, int current) {
	if (s.empty() || s == "current")
		return current;
	if (s == "center")
		return area_start + (area_len - len) / 2;
	if (auto p = percent(s, area_len))
		return area_start + *p;
	if (auto n = integer(s)) {
		if (s[0] == '-')
			return area_start + area_len - len + *n;
		return area_start + *n;
	}
	return current;
}

int size(const std::string &s, int area_len, int current) {
	if (s.empty() || s == "current")
		return current;
	if (auto p = percent(s, area_len))
		return std::max(1, *p);
	if (auto n = integer(s); n && *n > 0)
		return *n;
	return current;
}

bool overlaps_rows(const Rect &a, const Rect &b) {
	return a.y < b.y + b.height && b.y < a.y + a.height;
}
bool overlaps_cols(const Rect &a, const Rect &b) {
	return a.x < b.x + b.width && b.x < a.x + a.width;
}
bool alongside(const Rect &a, const Rect &b, EdgeDir dir) {
	return dir == EdgeDir::East || dir == EdgeDir::West ? overlaps_rows(a, b)
			: overlaps_cols(a, b);
}

int right(const Rect &r) { return r.x + r.width; }
int bottom(const Rect &r) { return r.y + r.height; }

}  // namespace

std::optional<EdgeDir> edge_dir_from_name(std::string_view n) {
	if (n == "north" || n == "up")
		return EdgeDir::North;
	if (n == "south" || n == "down")
		return EdgeDir::South;
	if (n == "east" || n == "right")
		return EdgeDir::East;
	if (n == "west" || n == "left")
		return EdgeDir::West;
	return std::nullopt;
}

std::optional<Dir8> dir8_from_name(std::string_view n) {
	static const std::pair<const char *, Dir8> names[] = {
		{"north", Dir8::North}, {"up", Dir8::North}, {"northeast", Dir8::NorthEast},
		{"east", Dir8::East}, {"right", Dir8::East}, {"southeast", Dir8::SouthEast},
		{"south", Dir8::South}, {"down", Dir8::South}, {"southwest", Dir8::SouthWest},
		{"west", Dir8::West}, {"left", Dir8::West}, {"northwest", Dir8::NorthWest}};
	for (const auto &[name, d] : names)
		if (n == name)
			return d;
	return std::nullopt;
}

std::optional<size_t> directional_target(const Rect &from, const std::vector<Rect> &cands,
		Dir8 dir) {
	/* Unit vector of the direction (diagonals: both axes). */
	int ux = 0, uy = 0;
	switch (dir) {
	case Dir8::North: uy = -1; break;
	case Dir8::South: uy = 1; break;
	case Dir8::East: ux = 1; break;
	case Dir8::West: ux = -1; break;
	case Dir8::NorthEast: ux = 1; uy = -1; break;
	case Dir8::SouthEast: ux = 1; uy = 1; break;
	case Dir8::SouthWest: ux = -1; uy = 1; break;
	case Dir8::NorthWest: ux = -1; uy = -1; break;
	}
	long cx = from.x + from.width / 2, cy = from.y + from.height / 2;
	std::optional<size_t> best;
	long best_score = 0;
	for (size_t i = 0; i < cands.size(); ++i) {
		const Rect &c = cands[i];
		long dx = c.x + c.width / 2 - cx, dy = c.y + c.height / 2 - cy;
		long ahead, aside;
		if (ux != 0 && uy != 0) {
			/* Diagonal: both ways, the nearer the better. */
			if (dx * ux <= 0 || dy * uy <= 0)
				continue;
			ahead = dx * ux + dy * uy;
			aside = std::labs(dx * ux - dy * uy);
		} else if (ux != 0) {
			if (dx * ux <= 0)
				continue;
			ahead = dx * ux;
			aside = std::labs(dy);
		} else {
			if (dy * uy <= 0)
				continue;
			ahead = dy * uy;
			aside = std::labs(dx);
		}
		long score = ahead + 2 * aside;
		if (!best || score < best_score) {
			best = i;
			best_score = score;
		}
	}
	return best;
}

Rect move_resize_to(const Rect &f, const Rect &area, const ActionArgs &args) {
	Rect r = f;
	r.width = size(arg(args, "width"), area.width, f.width);
	r.height = size(arg(args, "height"), area.height, f.height);
	r.x = position(arg(args, "x"), area.x, area.width, r.width, f.x);
	r.y = position(arg(args, "y"), area.y, area.height, r.height, f.y);
	return r;
}

Rect move_relative(const Rect &f, const ActionArgs &args) {
	Rect r = f;
	r.x += integer(arg(args, "x")).value_or(0);
	r.y += integer(arg(args, "y")).value_or(0);
	return r;
}

Rect resize_relative(const Rect &f, const ActionArgs &args) {
	int left = integer(arg(args, "left")).value_or(0);
	int rt = integer(arg(args, "right")).value_or(integer(arg(args, "x")).value_or(0));
	int top = integer(arg(args, "top")).value_or(0);
	int bot = integer(arg(args, "bottom")).value_or(integer(arg(args, "y")).value_or(0));
	Rect r{f.x - left, f.y - top, f.width + left + rt, f.height + top + bot};
	if (r.width < 1) {
		r.x = f.x;
		r.width = 1;
	}
	if (r.height < 1) {
		r.y = f.y;
		r.height = 1;
	}
	return r;
}

Rect move_to_center(const Rect &f, const Rect &area) {
	return {area.x + (area.width - f.width) / 2, area.y + (area.height - f.height) / 2,
			f.width, f.height};
}

Rect move_to_edge(const Rect &f, const Rect &area, const std::vector<Rect> &others,
		EdgeDir dir) {
	Rect r = f;
	/* The nearest edge (another window's either side, the area's) that one
	 * of the frame's edges reaches first, moving that way. */
	int best = std::numeric_limits<int>::max();
	auto consider = [&best](int distance) {
		if (distance > 0)
			best = std::min(best, distance);
	};
	switch (dir) {
	case EdgeDir::East:
		consider(right(area) - right(f));
		for (const Rect &o : others)
			if (alongside(f, o, dir)) {
				consider(o.x - right(f));
				consider(right(o) - right(f));
			}
		if (best != std::numeric_limits<int>::max())
			r.x += best;
		break;
	case EdgeDir::West:
		consider(f.x - area.x);
		for (const Rect &o : others)
			if (alongside(f, o, dir)) {
				consider(f.x - right(o));
				consider(f.x - o.x);
			}
		if (best != std::numeric_limits<int>::max())
			r.x -= best;
		break;
	case EdgeDir::South:
		consider(bottom(area) - bottom(f));
		for (const Rect &o : others)
			if (alongside(f, o, dir)) {
				consider(o.y - bottom(f));
				consider(bottom(o) - bottom(f));
			}
		if (best != std::numeric_limits<int>::max())
			r.y += best;
		break;
	case EdgeDir::North:
		consider(f.y - area.y);
		for (const Rect &o : others)
			if (alongside(f, o, dir)) {
				consider(f.y - bottom(o));
				consider(f.y - o.y);
			}
		if (best != std::numeric_limits<int>::max())
			r.y -= best;
		break;
	}
	return r;
}

Rect grow_to_edge(const Rect &f, const Rect &area, const std::vector<Rect> &others,
		EdgeDir dir) {
	Rect r = f;
	switch (dir) {
	case EdgeDir::East: {
		int edge = right(area);
		for (const Rect &o : others)
			if (alongside(f, o, dir) && o.x >= right(f))
				edge = std::min(edge, o.x);
		if (edge > right(f))
			r.width = edge - f.x;
		break;
	}
	case EdgeDir::West: {
		int edge = area.x;
		for (const Rect &o : others)
			if (alongside(f, o, dir) && right(o) <= f.x)
				edge = std::max(edge, right(o));
		if (edge < f.x) {
			r.width = right(f) - edge;
			r.x = edge;
		}
		break;
	}
	case EdgeDir::South: {
		int edge = bottom(area);
		for (const Rect &o : others)
			if (alongside(f, o, dir) && o.y >= bottom(f))
				edge = std::min(edge, o.y);
		if (edge > bottom(f))
			r.height = edge - f.y;
		break;
	}
	case EdgeDir::North: {
		int edge = area.y;
		for (const Rect &o : others)
			if (alongside(f, o, dir) && bottom(o) <= f.y)
				edge = std::max(edge, bottom(o));
		if (edge < f.y) {
			r.height = bottom(f) - edge;
			r.y = edge;
		}
		break;
	}
	}
	return r;
}

Rect shrink_to_edge(const Rect &f, const std::vector<Rect> &others, EdgeDir dir,
		int min_size) {
	Rect r = f;
	switch (dir) {
	case EdgeDir::East: {
		int edge = std::numeric_limits<int>::min();
		for (const Rect &o : others)
			if (alongside(f, o, dir))
				for (int e : {o.x, right(o)})
					if (e < right(f) && e >= f.x + min_size)
						edge = std::max(edge, e);
		if (edge != std::numeric_limits<int>::min())
			r.width = edge - f.x;
		break;
	}
	case EdgeDir::West: {
		int edge = std::numeric_limits<int>::max();
		for (const Rect &o : others)
			if (alongside(f, o, dir))
				for (int e : {o.x, right(o)})
					if (e > f.x && e <= right(f) - min_size)
						edge = std::min(edge, e);
		if (edge != std::numeric_limits<int>::max()) {
			r.width = right(f) - edge;
			r.x = edge;
		}
		break;
	}
	case EdgeDir::South: {
		int edge = std::numeric_limits<int>::min();
		for (const Rect &o : others)
			if (alongside(f, o, dir))
				for (int e : {o.y, bottom(o)})
					if (e < bottom(f) && e >= f.y + min_size)
						edge = std::max(edge, e);
		if (edge != std::numeric_limits<int>::min())
			r.height = edge - f.y;
		break;
	}
	case EdgeDir::North: {
		int edge = std::numeric_limits<int>::max();
		for (const Rect &o : others)
			if (alongside(f, o, dir))
				for (int e : {o.y, bottom(o)})
					if (e > f.y && e <= bottom(f) - min_size)
						edge = std::min(edge, e);
		if (edge != std::numeric_limits<int>::max()) {
			r.height = bottom(f) - edge;
			r.y = edge;
		}
		break;
	}
	}
	return r;
}

Rect grow_to_fill(const Rect &f, const Rect &area, const std::vector<Rect> &others) {
	/* Sideways first, then up and down: each side to the next edge that
	 * isn't already touching. */
	Rect r = f;
	for (EdgeDir d : {EdgeDir::West, EdgeDir::East, EdgeDir::North, EdgeDir::South})
		r = grow_to_edge(r, area, others, d);
	return r;
}

}  // namespace wb
