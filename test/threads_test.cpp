#include "wb_test.hpp"

#include <poll.h>
#include <pthread.h>
#include <signal.h>

#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

#include "waybox/threads.hpp"

using namespace std::chrono_literals;

namespace {

bool readable(int fd, int timeout_ms = 0) {
	struct pollfd p = {fd, POLLIN, 0};
	return poll(&p, 1, timeout_ms) == 1 && (p.revents & POLLIN);
}

}  // namespace

/* Posted from several threads at once: all of it runs, on the thread that
 * calls run(), and the fd says when there's something. */
WB_TEST(main_queue_from_many_threads) {
	wb::MainQueue q;
	WB_CHECK(!readable(q.fd()));
	std::vector<int> got;  /* only touched by the main thread */
	const auto main_id = std::this_thread::get_id();
	bool all_on_main = true;
	{
		std::vector<std::jthread> posters;
		for (int t = 0; t < 4; ++t)
			posters.emplace_back([&q, &got, &all_on_main, main_id, t] {
				for (int i = 0; i < 250; ++i)
					q.post([&got, &all_on_main, main_id, v = t * 1000 + i] {
						all_on_main = all_on_main && std::this_thread::get_id() == main_id;
						got.push_back(v);
					});
			});
	}
	WB_CHECK(readable(q.fd()));
	WB_CHECK(q.run() == 1000);
	WB_CHECK(got.size() == 1000 && all_on_main);
	WB_CHECK(std::set<int>(got.begin(), got.end()).size() == 1000);
	WB_CHECK(!readable(q.fd()));  /* drained */
	WB_CHECK(q.run() == 0);
}

/* Work posted while run() runs waits for the next call, the fd readable
 * for it; closed, waiting work is dropped and later posts too. */
WB_TEST(main_queue_post_during_run_and_close) {
	wb::MainQueue q;
	int ran = 0;
	q.post([&] {
		++ran;
		q.post([&] { ran += 10; });
	});
	WB_CHECK(q.run() == 1 && ran == 1);
	WB_CHECK(readable(q.fd()));
	WB_CHECK(q.run() == 1 && ran == 11);
	q.post([&] { ran += 100; });
	q.close();
	q.post([&] { ran += 1000; });
	WB_CHECK(q.run() == 0 && ran == 11);
}

/* Workers do the jobs off the main thread and hand results back through
 * the queue, which applies them on the main thread. */
WB_TEST(workers_hand_results_to_main) {
	wb::MainQueue q;
	std::atomic<int> off_main{0};
	const auto main_id = std::this_thread::get_id();
	long sum = 0;  /* main thread only */
	{
		wb::Workers w(3);
		WB_CHECK(w.count() == 3);
		for (int i = 1; i <= 200; ++i)
			w.submit([&q, &sum, &off_main, main_id, i](std::stop_token) {
				if (std::this_thread::get_id() != main_id)
					++off_main;
				long sq = static_cast<long>(i) * i;  /* the "work" */
				q.post([&sum, sq] { sum += sq; });
			});
		int waited = 0;
		size_t done = 0;
		while (done < 200 && waited++ < 500) {
			if (readable(q.fd(), 10))
				done += q.run();
		}
		WB_CHECK(done == 200);
	}
	WB_CHECK(off_main.load() == 200);
	WB_CHECK(sum == 200L * 201 * 401 / 6);
}

/* Stopping: a long job sees its stop token and gives up; jobs not started
 * are dropped; the destructor returns. */
WB_TEST(workers_stop) {
	std::atomic<bool> started{false}, gave_up{false};
	std::atomic<int> later{0};
	auto t0 = std::chrono::steady_clock::now();
	{
		wb::Workers w(1);
		w.submit([&](std::stop_token stop) {
			started = true;
			while (!stop.stop_requested())
				std::this_thread::sleep_for(1ms);
			gave_up = true;
		});
		for (int i = 0; i < 5; ++i)
			w.submit([&](std::stop_token) { ++later; });
		while (!started)
			std::this_thread::sleep_for(1ms);
	}
	WB_CHECK(gave_up.load());
	WB_CHECK(later.load() == 0);
	WB_CHECK(std::chrono::steady_clock::now() - t0 < 5s);
}

/* A result is applied only if nothing newer was asked for since. */
WB_TEST(generation) {
	wb::Generation g;
	uint64_t a = g.next();
	WB_CHECK(g.current(a));
	uint64_t b = g.next();
	WB_CHECK(!g.current(a) && g.current(b));
	WB_CHECK(wb::Workers::default_count() >= 1 && wb::Workers::default_count() <= 4);
}

/* A worker's thread blocks every signal, so SIGTERM and the rest reach the
 * main thread, where the compositor handles them. */
WB_TEST(workers_block_signals) {
	wb::MainQueue q;
	bool blocked = false;
	{
		wb::Workers w(1);
		w.submit([&q, &blocked](std::stop_token) {
			sigset_t now;
			pthread_sigmask(SIG_BLOCK, nullptr, &now);
			bool b = sigismember(&now, SIGTERM) == 1 && sigismember(&now, SIGINT) == 1 &&
					sigismember(&now, SIGCHLD) == 1;
			q.post([&blocked, b] { blocked = b; });
		});
		for (int i = 0; i < 200 && !readable(q.fd(), 10); ++i) {
		}
		q.run();
	}
	WB_CHECK(blocked);
}
