#include "wb_test.hpp"

#include <cairo.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <deque>
#include <functional>
#include <string>

#include "waybox/icon_loader.hpp"

namespace {

/* A scheduler run by hand: what's posted waits until run (or is dropped
 * with the scheduler). */
struct Stepped : wb::Scheduler {
	std::deque<std::function<void()>> q;
	void post(std::function<void()> work) override { q.push_back(std::move(work)); }
	size_t run() {
		size_t n = 0;
		while (!q.empty()) {
			auto f = std::move(q.front());
			q.pop_front();
			f();
			++n;
		}
		return n;
	}
};

/* An icon theme with one PNG icon, as XDG_DATA_HOME. */
struct Theme {
	std::string root;
	Theme() {
		char tmpl[] = "/tmp/wb-iconcache-XXXXXX";
		root = mkdtemp(tmpl);
		std::string dir = root;
		for (const char *part : {"/icons", "/hicolor", "/48x48", "/apps"}) {
			dir += part;
			mkdir(dir.c_str(), 0700);
		}
		cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 48, 48);
		cairo_t *cr = cairo_create(s);
		cairo_set_source_rgb(cr, 0.2, 0.4, 0.8);
		cairo_paint(cr);
		cairo_destroy(cr);
		cairo_surface_write_to_png(s, (dir + "/wb-test-icon.png").c_str());
		cairo_surface_destroy(s);
		setenv("XDG_DATA_HOME", root.c_str(), 1);
		setenv("XDG_DATA_DIRS", (root + "/none").c_str(), 1);
	}
	~Theme() {
		std::string cmd = "rm -rf '" + root + "'";
		if (system(cmd.c_str()) != 0) {}
	}
};

}  // namespace

/* Loaded on the pool, kept back on the main thread. */
WB_TEST(icon_preload_keeps_the_icon) {
	Theme theme;
	Stepped pool, main;
	wb::IconCache cache;
	size_t done = 99;
	cache.preload({"wb-test-icon"}, "hicolor", 48, pool, main, [&](size_t n) { done = n; });
	WB_CHECK(pool.run() == 1);  /* decoded */
	WB_CHECK(main.run() >= 1);  /* kept */
	WB_CHECK(done == 1);
	cairo_surface_t *got = cache.get("wb-test-icon", "hicolor", 48);  /* (a reference) */
	WB_CHECK(got != nullptr);
	if (got != nullptr)
		cairo_surface_destroy(got);
}

/* Cleared after the decode, before the main thread takes it: the decoded
 * surface is freed with the dropped task (LeakSanitizer says if not), and
 * nothing is kept. */
WB_TEST(icon_preload_cleared_after_decode) {
	Theme theme;
	Stepped pool, main;
	wb::IconCache cache;
	bool done = false;
	cache.preload({"wb-test-icon"}, "hicolor", 48, pool, main, [&](size_t) { done = true; });
	WB_CHECK(pool.run() == 1);
	cache.clear();
	main.run();
	WB_CHECK(!done);
	WB_CHECK(main.q.empty());
}

/* The same, with the resumption never run at all (dropped with its
 * scheduler, as at shutdown). */
WB_TEST(icon_preload_dropped_unrun) {
	Theme theme;
	wb::IconCache cache;
	{
		Stepped pool, main;
		cache.preload({"wb-test-icon"}, "hicolor", 48, pool, main, {});
		WB_CHECK(pool.run() == 1);
		WB_CHECK(!main.q.empty());
	}
}
