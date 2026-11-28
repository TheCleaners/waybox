#include "wb_test.hpp"

#include <poll.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "waybox/async.hpp"

using namespace std::chrono_literals;

namespace {

bool readable(int fd, int timeout_ms) {
	struct pollfd p = {fd, POLLIN, 0};
	return poll(&p, 1, timeout_ms) == 1 && (p.revents & POLLIN);
}

/* Runs the main queue until `done` or a deadline: the test's event loop. */
template <typename F>
bool pump(wb::MainQueue &q, F done, int ms = 5000) {
	auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (!done() && std::chrono::steady_clock::now() < end)
		if (readable(q.fd(), 10))
			q.run();
	return done();
}

struct Env {
	std::shared_ptr<wb::MainQueue> queue = std::make_shared<wb::MainQueue>();
	wb::Workers workers{2};
	wb::MainScheduler main{queue};
	wb::WorkerScheduler pool{workers};
};

wb::Task<int> square_off_main(Env &e, int x, std::thread::id main_id, bool &was_off) {
	co_await wb::on(e.pool);
	was_off = std::this_thread::get_id() != main_id;
	int r = x * x;  /* local data only, on the worker */
	co_await wb::on(e.main);
	co_return r;
}

wb::Task<void> sum_squares(Env &e, std::vector<int> xs, long &out, bool &all_off,
		bool &ended_on_main, std::thread::id main_id) {
	long sum = 0;
	all_off = true;
	for (int x : xs) {
		bool off = false;
		sum += co_await square_off_main(e, x, main_id, off);
		all_off = all_off && off;
	}
	out = sum;  /* owner state: only after coming back to the main thread */
	ended_on_main = std::this_thread::get_id() == main_id;
}

}  // namespace

/* A task hops to the workers and back, awaits another task for its value,
 * and touches its owner's state on the main thread. */
WB_TEST(task_hops_and_returns) {
	Env e;
	wb::TaskScope scope;
	long out = -1;
	bool all_off = false, on_main = false;
	scope.spawn(sum_squares(e, {1, 2, 3, 4}, out, all_off, on_main, std::this_thread::get_id()));
	WB_CHECK(pump(*e.queue, [&] { return out >= 0; }));
	WB_CHECK(out == 30 && all_off && on_main);
	WB_CHECK(pump(*e.queue, [&] { return scope.running() == 0; }));
}

namespace {
wb::Task<void> touch_later(Env &e, std::atomic<int> &reached, int &owner_field) {
	co_await wb::on(e.pool);
	reached = 1;  /* running on the worker now */
	std::this_thread::sleep_for(50ms);  /* the scope goes meanwhile */
	co_await wb::on(e.main);
	owner_field = 42;  /* must never run: the scope is gone */
	reached = 2;
}
}  // namespace

/* The scope gone while the task was on a worker: back on the main thread,
 * it's destroyed instead of resumed, and its owner's state isn't touched. */
WB_TEST(scope_gone_drops_task) {
	Env e;
	std::atomic<int> reached{0};
	int owner_field = 0;
	auto scope = std::make_unique<wb::TaskScope>();
	scope->spawn(touch_later(e, reached, owner_field));
	while (reached.load() < 1)
		std::this_thread::sleep_for(1ms);
	scope.reset();  /* while the task runs on the worker */
	pump(*e.queue, [] { return false; }, 200);
	WB_CHECK(reached.load() == 1);
	WB_CHECK(owner_field == 0);
}

namespace {
struct Counted {
	std::atomic<int> &alive;
	explicit Counted(std::atomic<int> &a) : alive(a) { ++alive; }
	~Counted() { --alive; }
};
wb::Task<void> waits_on_main(Env &e, std::atomic<int> &alive) {
	Counted c(alive);
	co_await wb::on(e.main);  /* posted, never run: the queue closes */
	co_await wb::on(e.main);
}
}  // namespace

/* A resumption dropped without running (the queue closed at shutdown)
 * destroys the task: its locals' destructors run, nothing leaks. */
WB_TEST(dropped_resumption_destroys_task) {
	std::atomic<int> alive{0};
	{
		Env e;
		wb::TaskScope scope;
		scope.spawn(waits_on_main(e, alive));
		WB_CHECK(alive.load() == 1 && scope.running() == 1);
		e.queue->close();
		WB_CHECK(alive.load() == 0 && scope.running() == 0);
	}
	WB_CHECK(alive.load() == 0);
}

namespace {
wb::Task<void> until_stopped(Env &e, std::atomic<int> &rounds, std::atomic<bool> &saw_stop) {
	while (true) {
		if (co_await wb::stop_requested()) {
			saw_stop = true;
			co_return;
		}
		++rounds;
		co_await wb::on(e.main);
	}
}
}  // namespace

/* A scope's stop request reaches a task, which finishes early. */
WB_TEST(stop_request) {
	Env e;
	wb::TaskScope scope;
	std::atomic<int> rounds{0};
	std::atomic<bool> saw_stop{false};
	scope.spawn(until_stopped(e, rounds, saw_stop));
	pump(*e.queue, [&] { return rounds.load() >= 5; });
	scope.request_stop();
	WB_CHECK(pump(*e.queue, [&] { return saw_stop.load(); }));
	WB_CHECK(scope.running() == 0);
}

namespace {
/* A scheduler with timers kept by the test: post_after waits in a list
 * until the test fires them. */
struct ManualTimers : wb::Scheduler {
	wb::MainQueue &q;
	std::vector<std::pair<int, std::function<void()>>> later;
	explicit ManualTimers(wb::MainQueue &queue) : q(queue) {}
	void post(std::function<void()> w) override { q.post(std::move(w)); }
	void post_after(std::chrono::milliseconds d, std::function<void()> w) override {
		later.emplace_back(static_cast<int>(d.count()), std::move(w));
	}
};
wb::Task<void> sleeper(ManualTimers &t, int &woke) {
	co_await wb::sleep_for(t, 100ms);
	woke = 1;
}
}  // namespace

/* sleep_for goes through the scheduler's timer, and resumes when it fires. */
WB_TEST(sleep_for_uses_timer) {
	auto q = std::make_shared<wb::MainQueue>();
	ManualTimers t(*q);
	wb::TaskScope scope;
	int woke = 0;
	scope.spawn(sleeper(t, woke));
	WB_CHECK(t.later.size() == 1 && t.later[0].first == 100 && woke == 0);
	t.later[0].second();
	WB_CHECK(woke == 1 && scope.running() == 0);
}

namespace {
wb::Task<void> many(Env &e, std::atomic<int> &done) {
	for (int i = 0; i < 20; ++i) {
		co_await wb::on(e.pool);
		co_await wb::on(e.main);
	}
	++done;
}
}  // namespace

/* Many tasks hopping at once (for ThreadSanitizer). */
WB_TEST(many_tasks) {
	Env e;
	wb::TaskScope scope;
	std::atomic<int> done{0};
	for (int i = 0; i < 16; ++i)
		scope.spawn(many(e, done));
	WB_CHECK(pump(*e.queue, [&] { return done.load() == 16; }, 10000));
	WB_CHECK(pump(*e.queue, [&] { return scope.running() == 0; }));
}
