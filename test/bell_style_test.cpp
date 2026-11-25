#include "wb_test.hpp"

#include "waybox/bell_style.hpp"

using wb::BellFlash;

WB_TEST(bell_style_parsed) {
	auto s = wb::parse_bell_style("", "", "", "", "");
	WB_CHECK(s.flash == BellFlash::None);
	WB_CHECK(wb::parse_bell_style("yes", "", "", "", "").flash == BellFlash::Window);
	WB_CHECK(wb::parse_bell_style("window", "", "", "", "").flash == BellFlash::Window);
	WB_CHECK(wb::parse_bell_style(" border ", "", "", "", "").flash == BellFlash::Border);
	WB_CHECK(wb::parse_bell_style("screen", "", "", "", "").flash == BellFlash::Screen);
	WB_CHECK(wb::parse_bell_style("no", "", "", "", "").flash == BellFlash::None);
	s = wb::parse_bell_style("border", "#ff880099", "200", "3", "6");
	WB_CHECK(s.color.r == 0xff && s.color.g == 0x88 && s.color.b == 0 && s.color.a == 0x99);
	WB_CHECK(s.duration_ms == 200 && s.count == 3 && s.width == 6);
	/* Clamped. */
	s = wb::parse_bell_style("window", "", "5", "99", "1000");
	WB_CHECK(s.duration_ms == 50 && s.count == 5 && s.width == 64);
	s = wb::parse_bell_style("window", "", "999999999", "0", "0");
	WB_CHECK(s.duration_ms == 2000 && s.count == 1 && s.width == 1);
	/* A note: longer by default, one only. */
	s = wb::parse_bell_style("osd", "", "", "4", "");
	WB_CHECK(s.flash == BellFlash::Osd && s.duration_ms == 1500 && s.count == 1);
	WB_CHECK(wb::parse_bell_style("osd", "", "8000", "", "").duration_ms == 8000);
	/* Unreadable: left at the default, and said. */
	std::vector<std::string> problems;
	s = wb::parse_bell_style("strobe", "reddish", "-5", "2.5", "x", &problems);
	WB_CHECK(s.flash == BellFlash::None && s.count == 1 && s.width == 4);
	WB_CHECK(problems.size() == 5);
	WB_CHECK(problems[0] == "flash=\"strobe\"");
}

WB_TEST(bell_blinks_never_faster_than_3_a_second) {
	auto s = wb::parse_bell_style("window", "", "50", "5", "");
	auto b = wb::blink_schedule(s);
	WB_CHECK(b.size() == 5);
	for (size_t i = 0; i < b.size(); ++i) {
		WB_CHECK(b[i].off_ms - b[i].on_ms == 50);
		if (i > 0)
			WB_CHECK(b[i].on_ms - b[i - 1].on_ms >= wb::kBellMinPeriodMs);
	}
	/* Long blinks: off as long as on. */
	b = wb::blink_schedule(wb::parse_bell_style("window", "", "400", "2", ""));
	WB_CHECK(b.size() == 2 && b[1].on_ms == 800 && b[1].off_ms == 1200);
	WB_CHECK(wb::blink_schedule(wb::parse_bell_style("no", "", "", "", "")).empty());
	WB_CHECK(wb::blink_schedule(wb::parse_bell_style("osd", "", "", "3", "")).size() == 1);
}

WB_TEST(bell_pacer) {
	auto one = wb::blink_schedule(wb::parse_bell_style("window", "", "150", "1", ""));
	wb::BellPacer p;
	WB_CHECK(p.allow(1000, one));
	WB_CHECK(!p.allow(1001, one));   /* a burst: let go */
	WB_CHECK(!p.allow(1300, one));   /* too soon after the last blink */
	WB_CHECK(p.allow(1334, one));
	/* Whatever comes in, never more than 3 starts in a second. */
	wb::BellPacer q;
	int shown = 0;
	for (uint32_t t = 0; t < 1000; t += 7)
		shown += q.allow(t, one);
	WB_CHECK(shown == 3);
	/* Not while a sequence is showing. */
	auto three = wb::blink_schedule(wb::parse_bell_style("window", "", "100", "3", ""));
	wb::BellPacer r;
	WB_CHECK(r.allow(0, three));
	WB_CHECK(!r.allow(700, three));  /* its third blink is at 668..768 */
	WB_CHECK(!r.allow(900, three));  /* < 334 after that blink's start */
	WB_CHECK(r.allow(1002, three));
	/* Across the clock's wrap. */
	wb::BellPacer w;
	WB_CHECK(w.allow(0xffffff00u, one));
	WB_CHECK(!w.allow(0xffffff10u, one));
	WB_CHECK(w.allow(0xffffff00u + 400u, one));
	/* Nothing to show: not "shown". */
	WB_CHECK(!wb::BellPacer().allow(0, {}));
}

WB_TEST(bell_style_overrides) {
	auto base = wb::parse_bell_style("border", "#ff0000", "200", "2", "8");
	/* Only what's given changes. */
	auto s = wb::bell_style_with(base, {{"color", "#00ff0080"}});
	WB_CHECK(s.flash == BellFlash::Border && s.duration_ms == 200 && s.count == 2 &&
			s.width == 8 && s.color.g == 0xff && s.color.a == 0x80);
	/* To a note: its own default duration, unless given. */
	s = wb::bell_style_with(base, {{"flash", "osd"}});
	WB_CHECK(s.flash == BellFlash::Osd && s.duration_ms == 1500 && s.count == 1);
	s = wb::bell_style_with(base, {{"flash", "osd"}, {"duration", "3000"}});
	WB_CHECK(s.duration_ms == 3000);
	/* Back from a note to a flash: a flash's default, not 1500. */
	auto note = wb::parse_bell_style("osd", "", "", "", "");
	WB_CHECK(wb::bell_style_with(note, {{"flash", "window"}}).duration_ms == 150);
	/* A long note duration can't carry over into a flash. */
	auto long_note = wb::parse_bell_style("osd", "", "9000", "", "");
	WB_CHECK(wb::bell_style_with(long_note, {{"flash", "screen"}}).duration_ms == 150);
	/* Off, and bad values left as they were. */
	WB_CHECK(wb::bell_style_with(base, {{"flash", "no"}}).flash == BellFlash::None);
	std::vector<std::string> problems;
	s = wb::bell_style_with(base, {{"count", "lots"}, {"flash", "disco"}}, &problems);
	WB_CHECK(s == base && problems.size() == 2);
	WB_CHECK(wb::bell_style_with(base, {}) == base);
}
