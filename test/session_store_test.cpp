#include "wb_test.hpp"

#include <algorithm>
#include <limits>
#include <set>

#include "waybox/session_store.hpp"

using wb::SavedWindow;
using wb::SessionStore;

namespace {

SavedWindow window(int x, int y, int w, int h, const char *output) {
	SavedWindow s;
	s.x = x;
	s.y = y;
	s.width = w;
	s.height = h;
	s.output = output;
	return s;
}

}  // namespace

WB_TEST(store_round_trip) {
	SessionStore a;
	SavedWindow w = window(10, 20, 800, 600, "DP-1");
	w.maximized = true;
	w.desktop = 2;
	w.omnipresent = true;
	WB_CHECK(a.set_window("abc", "main", w));
	WB_CHECK(a.set_window("abc", "prefs \"quoted\" \xc3\xa9", window(0, 0, 300, 200, "HDMI-A-1")));
	a.sessions["abc"].used = 1700000000;
	SessionStore b;
	WB_CHECK(b.from_json(a.to_json()));
	WB_CHECK(b.sessions == a.sessions);
}

WB_TEST(store_rejects_junk) {
	SessionStore s;
	WB_CHECK(!s.from_json("not json"));
	WB_CHECK(!s.from_json("[1,2]"));
	WB_CHECK(!s.from_json("{\"version\":1}"));
	/* Bad entries are skipped, good ones kept. */
	WB_CHECK(s.from_json(R"({"sessions":{
		"ok":{"used":5,"windows":{"a":{"x":1,"y":2,"width":30,"height":40,"output":"X"},
		                          "zero":{"width":0,"height":10},
		                          "notobj":3}},
		"":{"windows":{}},
		"bad":7}})"));
	WB_CHECK(s.sessions.size() == 1);
	WB_CHECK(s.sessions["ok"].windows.size() == 1);
	WB_CHECK(s.sessions["ok"].windows["a"] == window(1, 2, 30, 40, "X"));
	WB_CHECK(s.sessions["ok"].used == 5);
}

WB_TEST(store_limits) {
	SessionStore s;
	std::string long_name(SessionStore::kMaxNameBytes + 1, 'n');
	WB_CHECK(!s.set_window("id", long_name, window(0, 0, 10, 10, "X")));
	for (size_t i = 0; i < SessionStore::kMaxWindows; ++i)
		WB_CHECK(s.set_window("id", "w" + std::to_string(i), window(0, 0, 10, 10, "X")));
	WB_CHECK(!s.set_window("id", "one-more", window(0, 0, 10, 10, "X")));
	/* Updating one that's there still works at the limit. */
	WB_CHECK(s.set_window("id", "w0", window(5, 5, 10, 10, "X")));
}

WB_TEST(store_expiry) {
	SessionStore s;
	int64_t now = 1'000'000'000;
	s.sessions["old"].used = now - SessionStore::kMaxAgeSeconds - 1;
	s.sessions["recent"].used = now - 10;
	s.expire(now);
	WB_CHECK(s.sessions.count("old") == 0 && s.sessions.count("recent") == 1);
	/* Over the count: the least recently used go. */
	s.sessions.clear();
	for (size_t i = 0; i < SessionStore::kMaxSessions + 5; ++i)
		s.sessions["s" + std::to_string(i)].used = now - static_cast<int64_t>(i);
	s.expire(now);
	WB_CHECK(s.sessions.size() == SessionStore::kMaxSessions);
	WB_CHECK(s.sessions.count("s0") == 1);
	WB_CHECK(s.sessions.count("s" + std::to_string(SessionStore::kMaxSessions + 4)) == 0);
}

/* Timestamps checked before they're converted: out of int64_t's range,
 * negative or not whole, a session counts as long unused (and expires);
 * expire() never subtracts an unchecked one. (Run under UBSan: a cast or
 * an overflow here would stop the test.) */
WB_TEST(store_bad_timestamps) {
	SessionStore s;
	const int64_t now = 1'700'000'000;
	WB_CHECK(s.from_json(R"({"version":1,"sessions":{
		"huge":{"used":1e30,"windows":{}},
		"two63":{"used":9223372036854775808,"windows":{}},
		"neg":{"used":-5,"windows":{}},
		"half":{"used":1700000000.5,"windows":{}},
		"str":{"used":"soon","windows":{}},
		"good":{"used":1699999000,"windows":{}}}})"));
	WB_CHECK(s.sessions.size() == 6);
	for (const char *bad : {"huge", "two63", "neg", "half", "str"})
		WB_CHECK(s.sessions[bad].used == 0);
	WB_CHECK(s.sessions["good"].used == 1699999000);
	s.expire(now);
	WB_CHECK(s.sessions.size() == 1 && s.sessions.count("good") == 1);
	/* Set in code to the extremes: compared, not subtracted. */
	s.sessions["min"].used = std::numeric_limits<int64_t>::min();
	s.sessions["max"].used = std::numeric_limits<int64_t>::max();
	s.expire(now);
	WB_CHECK(s.sessions.count("min") == 0 && s.sessions.count("max") == 1);
	s.expire(std::numeric_limits<int64_t>::min());
	s.expire(std::numeric_limits<int64_t>::max());
	WB_CHECK(s.sessions.count("max") == 1);
}

/* The format's version: none is the first; one this Waybox doesn't know
 * isn't read (a newer one says so, so it isn't written over); what was
 * loaded before stays when a load fails. */
WB_TEST(store_versions) {
	SessionStore s;
	bool newer = true;
	WB_CHECK(s.from_json(R"({"sessions":{"a":{"used":5,"windows":{}}}})", &newer) && !newer);
	WB_CHECK(s.from_json(R"({"version":1,"sessions":{"b":{"used":5,"windows":{}}}})", &newer));
	WB_CHECK(s.sessions.count("b") == 1 && s.sessions.count("a") == 0);
	WB_CHECK(!s.from_json(R"({"version":2,"sessions":{}})", &newer) && newer);
	WB_CHECK(!s.from_json(R"({"version":"1","sessions":{}})", &newer) && !newer);
	WB_CHECK(!s.from_json(R"({"version":0,"sessions":{}})", &newer) && !newer);
	WB_CHECK(!s.from_json("not json"));
	WB_CHECK(s.sessions.size() == 1 && s.sessions.count("b") == 1);
	/* Bigger than a store can be: not parsed at all. */
	std::string big = R"({"sessions":{},"pad":")" +
			std::string(SessionStore::kMaxFileBytes, 'x') + R"("})";
	WB_CHECK(!s.from_json(big));
	WB_CHECK(s.sessions.count("b") == 1);
}

/* Over the count, the newest kept: the same ones every time (ties by id). */
WB_TEST(store_trim_deterministic) {
	SessionStore a, b;
	const int64_t now = 1'700'000'000;
	for (size_t i = 0; i < SessionStore::kMaxSessions + 50; ++i) {
		std::string id = "s" + std::to_string(i);
		int64_t used = now - static_cast<int64_t>(i % 7);  /* many ties */
		a.sessions[id].used = used;
		b.sessions[id].used = used;
	}
	a.expire(now);
	b.expire(now);
	WB_CHECK(a.sessions.size() == SessionStore::kMaxSessions);
	WB_CHECK(a.sessions == b.sessions);
	/* None kept is older than one that went. */
	int64_t oldest_kept = now;
	for (const auto &[id, s] : a.sessions)
		oldest_kept = std::min(oldest_kept, s.used);
	WB_CHECK(oldest_kept >= now - 6);
}

WB_TEST(restored_rect_on_its_output) {
	std::vector<wb::OutputBox> outs = {{"DP-1", {0, 0, 1920, 1080}},
			{"HDMI-A-1", {1920, 30, 1280, 994}}};
	/* Relative to its output, wherever that is now. */
	auto r = wb::restored_rect(window(100, 50, 800, 600, "HDMI-A-1"), outs);
	WB_CHECK(r && r->x == 2020 && r->y == 80 && r->width == 800 && r->height == 600);
	/* Moved back in when it no longer fits (a smaller output now). */
	r = wb::restored_rect(window(1000, 900, 800, 600, "HDMI-A-1"), outs);
	WB_CHECK(r && r->x == 1920 + 1280 - 800 && r->y == 30 + 994 - 600);
	/* Bigger than the output: its size. */
	r = wb::restored_rect(window(0, 0, 5000, 5000, "DP-1"), outs);
	WB_CHECK(r && r->width == 1920 && r->height == 1080 && r->x == 0 && r->y == 0);
	/* Its output gone: placed as new. */
	WB_CHECK(!wb::restored_rect(window(0, 0, 100, 100, "eDP-1"), outs));
	WB_CHECK(!wb::restored_rect(window(0, 0, 100, 100, ""), outs));
}

WB_TEST(session_ids) {
	std::set<std::string> ids;
	for (int i = 0; i < 100; ++i) {
		std::string id = wb::new_session_id();
		WB_CHECK(id.size() == 32);
		WB_CHECK(id.find_first_not_of("0123456789abcdef") == std::string::npos);
		ids.insert(id);
	}
	WB_CHECK(ids.size() == 100);
}

WB_TEST(utf8_validation) {
	WB_CHECK(wb::valid_utf8("plain"));
	WB_CHECK(wb::valid_utf8("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"));
	WB_CHECK(wb::valid_utf8(""));
	WB_CHECK(!wb::valid_utf8(std::string("a\0b", 3)));
	WB_CHECK(!wb::valid_utf8("\xc3"));            /* cut short */
	WB_CHECK(!wb::valid_utf8("\xe2\x82"));
	WB_CHECK(!wb::valid_utf8("\xc0\xaf"));        /* overlong */
	WB_CHECK(!wb::valid_utf8("\xed\xa0\x80"));    /* surrogate */
	WB_CHECK(!wb::valid_utf8("\xf4\x90\x80\x80")); /* past U+10FFFF */
	WB_CHECK(!wb::valid_utf8("\xff"));
	WB_CHECK(!wb::valid_utf8("\x80"));
}
