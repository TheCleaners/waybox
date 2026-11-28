#include "wb_test.hpp"

#include <poll.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "waybox/glib_thread.hpp"

using namespace std::chrono_literals;

namespace {

bool readable(int fd, int timeout_ms) {
	struct pollfd p = {fd, POLLIN, 0};
	return poll(&p, 1, timeout_ms) == 1 && (p.revents & POLLIN);
}

template <typename F>
bool pump(wb::MainQueue &q, F done, int ms = 5000) {
	auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (!done() && std::chrono::steady_clock::now() < end)
		if (readable(q.fd(), 10))
			q.run();
	return done();
}

wb::Task<void> via_glib(wb::GLibThread &g, wb::MainScheduler &main, std::thread::id main_id,
		bool &glib_ok, bool &back_on_main) {
	co_await wb::on(g);
	glib_ok = std::this_thread::get_id() != main_id &&
			g_main_context_get_thread_default() == g.context();
	co_await wb::on(main);
	back_on_main = std::this_thread::get_id() == main_id;
}

struct Counted {
	std::atomic<int> &alive;
	explicit Counted(std::atomic<int> &a) : alive(a) { ++alive; }
	~Counted() { --alive; }
};

wb::Task<void> waits_long(wb::GLibThread &g, std::atomic<int> &alive) {
	Counted c(alive);
	co_await wb::sleep_for(g, 60s);  /* never fires: the thread goes first */
}

}  // namespace

/* Work posted runs on the GLib thread, its context the thread's default;
 * a task hops there and back. */
WB_TEST(glib_thread_runs_tasks) {
	auto q = std::make_shared<wb::MainQueue>();
	wb::MainScheduler main(q);
	wb::GLibThread g;
	wb::TaskScope scope;
	bool glib_ok = false, back = false;
	scope.spawn(via_glib(g, main, std::this_thread::get_id(), glib_ok, back));
	WB_CHECK(pump(*q, [&] { return back; }));
	WB_CHECK(glib_ok && back);
}

/* post_after waits at least that long. */
WB_TEST(glib_thread_post_after) {
	wb::GLibThread g;
	std::atomic<bool> ran{false};
	auto t0 = std::chrono::steady_clock::now();
	std::atomic<long> waited{0};
	g.post_after(50ms, [&] {
		waited = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - t0).count();
		ran = true;
	});
	for (int i = 0; i < 300 && !ran; ++i)
		std::this_thread::sleep_for(5ms);
	WB_CHECK(ran.load() && waited.load() >= 45);
}

/* Destroyed with work still queued (a task sleeping on it): the work is
 * dropped, not run, and the task destroyed. */
WB_TEST(glib_thread_drops_on_destruction) {
	std::atomic<int> alive{0};
	wb::TaskScope scope;
	{
		wb::GLibThread g;
		scope.spawn(waits_long(g, alive));
		WB_CHECK(alive.load() == 1 && scope.running() == 1);
	}
	WB_CHECK(alive.load() == 0 && scope.running() == 0);
}
