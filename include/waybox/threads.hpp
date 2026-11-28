#ifndef WB_THREADS_HPP
#define WB_THREADS_HPP

/*
 * Threads for work that would block the compositor (drawing a menu, D-Bus
 * for agent tools), and the way their results come back.
 *
 * The rule: a worker never touches Wayland, wlroots or compositor objects.
 * It takes plain data in (copied, or owned by the job), and hands its
 * result back to the main thread with MainQueue::post, where it's applied.
 * Something that may have gone meanwhile (a menu closed while it was being
 * drawn) is checked there, on the main thread, by a Generation.
 *
 * Signals belong to the main thread: the compositor handles SIGTERM and
 * the rest through its event loop, which only works if no other thread
 * can receive them. A thread of Waybox's own blocks them all first
 * (block_signals), or a signal may land on it and kill the compositor.
 *
 * Pure C++20 (no wlroots), unit-tested under ThreadSanitizer
 * (test/threads_test.cpp, -Dtsan=true). The compositor reads MainQueue's
 * fd on its event loop (server.cpp).
 */
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace wb {

/* The main thread's mailbox: any thread posts work, the main thread runs
 * it when fd() becomes readable. */
class MainQueue {
public:
	MainQueue();
	~MainQueue();
	MainQueue(const MainQueue &) = delete;
	MainQueue &operator=(const MainQueue &) = delete;

	/* Readable while work is waiting (an eventfd). */
	int fd() const { return fd_; }
	/* From any thread. */
	void post(std::function<void()> work);
	/* On the main thread: runs what's waiting, in the order posted, and
	 * returns how many ran. Work posted while it runs waits for the next
	 * call (the fd stays readable for it). */
	size_t run();
	/* Shutting down: what's waiting is dropped, not run, and later posts
	 * are dropped too (a worker finishing after the compositor's objects
	 * went has nothing to apply its result to). */
	void close();

private:
	int fd_ = -1;
	std::mutex mutex_;
	std::deque<std::function<void()>> waiting_;
	bool closed_ = false;
};

/* A fixed pool of worker threads. A job gets the pool's stop token: long
 * work checks it and gives up when the pool is stopping. */
class Workers {
public:
	using Job = std::function<void(std::stop_token)>;

	explicit Workers(unsigned count = default_count());
	/* Stops the threads (each finishes or gives up its current job; jobs
	 * not started are dropped) and joins them. */
	~Workers();
	Workers(const Workers &) = delete;
	Workers &operator=(const Workers &) = delete;

	/* From any thread; ignored once stopping. Jobs mustn't throw. */
	void submit(Job job);
	/* Half the machine's threads, at least 1 and at most 4: work that
	 * leaves the compositor's thread free, not work that competes with
	 * the clients for the machine. */
	static unsigned default_count();
	unsigned count() const { return static_cast<unsigned>(threads_.size()); }

private:
	void run(std::stop_token stop);

	std::mutex mutex_;
	std::condition_variable_any ready_;
	std::deque<Job> jobs_;
	std::vector<std::jthread> threads_;
};

/* Blocks every signal on the calling thread: the first thing any thread
 * of Waybox's own does (Workers' threads do it themselves). */
void block_signals();

/* Which request is the current one: a result carries the generation it
 * was asked for, and is applied only if nothing newer was asked since. */
class Generation {
public:
	uint64_t next() { return ++n_; }
	bool current(uint64_t g) const { return n_.load() == g; }

private:
	std::atomic<uint64_t> n_{0};
};

}  // namespace wb

#endif /* WB_THREADS_HPP */
