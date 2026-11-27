#include "waybox/session_store.hpp"

#include <algorithm>
#include <vector>
#include <limits>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <random>
#include <unistd.h>

#include "waybox/json.hpp"

namespace wb {

namespace {

int number(const json::Value &o, const char *key, int fallback = 0) {
	const json::Value *v = o.get(key);
	const double *d = v != nullptr ? v->number() : nullptr;
	if (d == nullptr || *d != *d || *d > 1e9 || *d < -1e9)
		return fallback;
	return static_cast<int>(*d);
}

bool flag(const json::Value &o, const char *key) {
	const json::Value *v = o.get(key);
	const bool *b = v != nullptr ? v->boolean() : nullptr;
	return b != nullptr && *b;
}

}  // namespace

std::string SessionStore::to_json() const {
	json::Object root;
	root["version"] = 1;
	json::Object all;
	for (const auto &[id, s] : sessions) {
		json::Object windows;
		for (const auto &[name, w] : s.windows) {
			json::Object o;
			o["x"] = w.x;
			o["y"] = w.y;
			o["width"] = w.width;
			o["height"] = w.height;
			o["output"] = w.output;
			o["maximized"] = w.maximized;
			o["fullscreen"] = w.fullscreen;
			o["desktop"] = w.desktop;
			o["omnipresent"] = w.omnipresent;
			windows[name] = json::Value(std::move(o));
		}
		json::Object so;
		so["used"] = static_cast<double>(s.used);
		so["windows"] = json::Value(std::move(windows));
		all[id] = json::Value(std::move(so));
	}
	root["sessions"] = json::Value(std::move(all));
	return json::dump(json::Value(std::move(root))) + "\n";
}

bool SessionStore::from_json(std::string_view text, bool *newer) {
	if (newer != nullptr)
		*newer = false;
	if (text.size() > kMaxFileBytes)
		return false;
	std::optional<json::Value> v = json::parse(text);
	if (!v || v->object() == nullptr)
		return false;
	/* No version: the first format (as written before it said so). */
	if (const json::Value *ver = v->get("version")) {
		const double *n = ver->number();
		if (n == nullptr || *n != kVersion) {
			if (n != nullptr && *n > kVersion && newer != nullptr)
				*newer = true;
			return false;
		}
	}
	const json::Value *all = v->get("sessions");
	if (all == nullptr || all->object() == nullptr)
		return false;
	std::map<std::string, SavedSession> loaded;
	for (const auto &[id, so] : *all->object()) {
		if (so.object() == nullptr || id.empty() || id.size() > kMaxNameBytes || !valid_utf8(id))
			continue;
		SavedSession s;
		/* Checked before converting: a double out of int64_t's range
		 * (1e30) or not whole is no timestamp; such a session counts as
		 * long unused. */
		const json::Value *used = so.get("used");
		const double *u = used != nullptr ? used->number() : nullptr;
		if (u != nullptr && *u >= 0 && *u <= static_cast<double>(kMaxTimestamp) &&
				*u == std::floor(*u))
			s.used = static_cast<int64_t>(*u);
		const json::Value *windows = so.get("windows");
		if (windows != nullptr && windows->object() != nullptr)
			for (const auto &[name, wo] : *windows->object()) {
				if (wo.object() == nullptr || name.size() > kMaxNameBytes ||
						s.windows.size() >= kMaxWindows)
					continue;
				SavedWindow w;
				w.x = number(wo, "x");
				w.y = number(wo, "y");
				w.width = number(wo, "width");
				w.height = number(wo, "height");
				w.output = wo.str("output");
				w.maximized = flag(wo, "maximized");
				w.fullscreen = flag(wo, "fullscreen");
				w.desktop = std::max(0, number(wo, "desktop"));
				w.omnipresent = flag(wo, "omnipresent");
				if (w.width <= 0 || w.height <= 0)
					continue;
				s.windows[name] = w;
			}
		loaded[id] = std::move(s);
	}
	sessions = std::move(loaded);
	return true;
}

void SessionStore::expire(int64_t now) {
	/* Compared with a cutoff, never subtracted from: `used` may be anything
	 * (set in code, or a bad file's). */
	int64_t cutoff = now > std::numeric_limits<int64_t>::min() + kMaxAgeSeconds
			? now - kMaxAgeSeconds : std::numeric_limits<int64_t>::min();
	for (auto it = sessions.begin(); it != sessions.end();)
		it = it->second.used < cutoff ? sessions.erase(it) : std::next(it);
	if (sessions.size() <= kMaxSessions)
		return;
	/* The kMaxSessions most recently used kept, in one pass (ties by id,
	 * so which go is the same every time). */
	std::vector<std::pair<int64_t, std::string>> order;
	order.reserve(sessions.size());
	for (const auto &[id, s] : sessions)
		order.emplace_back(s.used, id);
	auto newer_first = [](const auto &a, const auto &b) {
		return a.first != b.first ? a.first > b.first : a.second < b.second;
	};
	std::nth_element(order.begin(), order.begin() + kMaxSessions, order.end(), newer_first);
	for (auto it = order.begin() + kMaxSessions; it != order.end(); ++it)
		sessions.erase(it->second);
}

bool SessionStore::set_window(const std::string &session, const std::string &name,
		const SavedWindow &w) {
	if (name.size() > kMaxNameBytes || session.size() > kMaxNameBytes)
		return false;
	SavedSession &s = sessions[session];
	if (!s.windows.count(name) && s.windows.size() >= kMaxWindows)
		return false;
	s.windows[name] = w;
	return true;
}

std::optional<Rect> restored_rect(const SavedWindow &w, const std::vector<OutputBox> &outputs) {
	for (const OutputBox &o : outputs) {
		if (o.name != w.output || w.output.empty())
			continue;
		Rect r{o.box.x + w.x, o.box.y + w.y, std::min(w.width, o.box.width),
				std::min(w.height, o.box.height)};
		/* Inside the output: moved back in where it no longer fits. */
		r.x = std::clamp(r.x, o.box.x, o.box.x + o.box.width - r.width);
		r.y = std::clamp(r.y, o.box.y, o.box.y + o.box.height - r.height);
		return r;
	}
	return std::nullopt;
}

std::string new_session_id() {
	unsigned char bytes[16] = {};
	bool got = false;
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		got = read(fd, bytes, sizeof bytes) == static_cast<ssize_t>(sizeof bytes);
		close(fd);
	}
	if (!got) {
		std::random_device rd;
		for (unsigned char &b : bytes)
			b = static_cast<unsigned char>(rd());
	}
	char hex[33];
	for (size_t i = 0; i < sizeof bytes; ++i)
		snprintf(hex + 2 * i, 3, "%02x", bytes[i]);
	return std::string(hex, 32);
}

bool valid_utf8(std::string_view s) {
	size_t i = 0;
	while (i < s.size()) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		size_t n;
		uint32_t cp;
		if (c == 0)
			return false;
		if (c < 0x80) {
			++i;
			continue;
		} else if ((c & 0xe0) == 0xc0) {
			n = 1;
			cp = c & 0x1f;
		} else if ((c & 0xf0) == 0xe0) {
			n = 2;
			cp = c & 0x0f;
		} else if ((c & 0xf8) == 0xf0) {
			n = 3;
			cp = c & 0x07;
		} else {
			return false;
		}
		for (size_t k = 1; k <= n; ++k) {
			if (i + k >= s.size())
				return false;
			unsigned char cc = static_cast<unsigned char>(s[i + k]);
			if ((cc & 0xc0) != 0x80)
				return false;
			cp = (cp << 6) | (cc & 0x3f);
		}
		/* Overlong forms, surrogates, past U+10FFFF. */
		if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
				(cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff)
			return false;
		i += n + 1;
	}
	return true;
}

}  // namespace wb
