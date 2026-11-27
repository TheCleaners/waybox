#include "wb_test.hpp"

#include "waybox/conditions.hpp"

using wb::Query;
using wb::WindowFacts;

namespace {

WindowFacts firefox() {
	WindowFacts w;
	w.title = "Waybox - Mozilla Firefox";
	w.app_id = "firefox";
	w.tag = "main";
	w.desktop = 1;
	w.monitor = 2;
	return w;
}

bool one(const Query &q, const WindowFacts *w, wb::DesktopFacts d = {1, 0},
		const WindowFacts *focus = nullptr) {
	return wb::conditions_true({q}, w, focus, d);
}

}  // namespace

WB_TEST(text_match_kinds) {
	WB_CHECK(wb::text_matches(wb::make_text_match("", "*Firefox"), "Waybox - Mozilla Firefox"));
	WB_CHECK(!wb::text_matches(wb::make_text_match("pattern", "Firefox"), "Mozilla Firefox"));
	WB_CHECK(wb::text_matches(wb::make_text_match("regex", "Fire(fox)?$"), "Mozilla Firefox"));
	WB_CHECK(wb::text_matches(wb::make_text_match("regex", "zil"), "Mozilla"));  /* anywhere */
	WB_CHECK(!wb::text_matches(wb::make_text_match("regex", "^zil"), "Mozilla"));
	WB_CHECK(wb::text_matches(wb::make_text_match("exact", "foot"), "foot"));
	WB_CHECK(!wb::text_matches(wb::make_text_match("exact", "foot"), "footclient"));
	/* An invalid regex matches nothing (and doesn't throw). */
	WB_CHECK(!wb::text_matches(wb::make_text_match("regex", "(unclosed"), "(unclosed"));
	/* Not asked: always true. */
	WB_CHECK(wb::text_matches(wb::TextMatch{}, "anything"));
}

WB_TEST(window_conditions) {
	WindowFacts w = firefox();
	Query q;
	WB_CHECK(one(q, &w));  /* no conditions: true */
	q.title = wb::make_text_match("regex", "Firefox$");
	q.cls = wb::make_text_match("", "fire*");
	q.name = wb::make_text_match("exact", "firefox");  /* class and name: the app id */
	q.role = wb::make_text_match("exact", "main");     /* role: the tag */
	q.type = wb::make_text_match("exact", "normal");
	WB_CHECK(one(q, &w));
	q.type = wb::make_text_match("exact", "dialog");
	WB_CHECK(!one(q, &w));

	Query s;
	s.maximized = false;
	WB_CHECK(one(s, &w));
	w.maximized_horizontal = true;
	WB_CHECK(one(s, &w));  /* maximized: in both directions */
	w.maximized_vertical = true;
	WB_CHECK(!one(s, &w));
	Query h;
	h.maximized_horizontal = true;
	h.undecorated = false;
	h.monitor = 2;
	WB_CHECK(one(h, &w));
	h.monitor = 1;
	WB_CHECK(!one(h, &w));
	Query flags;
	flags.shaded = false;
	flags.iconified = false;
	flags.fullscreen = false;
	flags.urgent = false;
	flags.focusable = true;
	WB_CHECK(one(flags, &w));
	w.fullscreen = true;
	WB_CHECK(!one(flags, &w));
}

WB_TEST(desktop_conditions) {
	WindowFacts w = firefox();  /* on desktop 2 (index 1) */
	Query q;
	q.desktop = Query::Desktop::Current;
	WB_CHECK(one(q, &w, {1, 0}));
	WB_CHECK(!one(q, &w, {0, 1}));
	q.desktop = Query::Desktop::Other;
	WB_CHECK(one(q, &w, {0, 1}));
	q.desktop = Query::Desktop::Last;
	WB_CHECK(one(q, &w, {0, 1}));
	WB_CHECK(!one(q, &w, {0, 2}));
	q.desktop = Query::Desktop::Number;
	q.desktop_number = 2;
	WB_CHECK(one(q, &w, {0, 0}));
	q.desktop_number = 3;
	WB_CHECK(!one(q, &w, {0, 0}));
	w.omnipresent = true;  /* on every desktop */
	WB_CHECK(one(q, &w, {0, 0}));
	Query a;
	a.active_desktop = 2;
	WB_CHECK(one(a, &w, {1, 0}));
	WB_CHECK(!one(a, &w, {0, 0}));
}

WB_TEST(no_window_and_focus_target) {
	Query q;
	q.title = wb::make_text_match("", "*");
	WB_CHECK(!one(q, nullptr));  /* about a window, and there's none */
	Query a;
	a.active_desktop = 2;
	a.fullscreen = true;  /* passed over when there's no window, as Openbox */
	WB_CHECK(one(a, nullptr, {1, 0}));
	WB_CHECK(!one(a, nullptr, {0, 0}));
	/* target="focus": asks about the focused window, not the action's. */
	WindowFacts target = firefox(), focused = firefox();
	focused.app_id = "foot";
	Query f;
	f.focus_target = true;
	f.cls = wb::make_text_match("exact", "foot");
	WB_CHECK(one(f, &target, {1, 0}, &focused));
	WB_CHECK(!one(f, &target, {1, 0}, &target));
	WB_CHECK(!one(f, &target, {1, 0}, nullptr));
	/* All queries must hold. */
	Query mine;
	mine.cls = wb::make_text_match("exact", "firefox");
	WB_CHECK(wb::conditions_true({mine, f}, &target, &focused, {1, 0}));
	WB_CHECK(!wb::conditions_true({mine, f}, &target, &target, {1, 0}));
}
