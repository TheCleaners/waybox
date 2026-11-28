#ifndef WB_ASYNC_HPP
#define WB_ASYNC_HPP

/*
 * Async work in Waybox: a few schedulers (where work runs) and coroutine
 * tasks that move between them, instead of a thread per job.
 *
 *   Task<void> preload(Icons &icons, std::string name) {
 *       co_await on(workers);          // off the compositor's thread
 *       auto surface = load(name);     // only local data here
 *       co_await on(main);             // back: owner state is safe
 *       icons.keep(name, surface);
 *   }
 *   scope.spawn(preload(icons, "firefox"));
 *
 * Schedulers: the compositor's thread (MainQueue), the worker pool
 * (Workers), one shared GLib thread (glib_thread.hpp). Named after C++26's
 * std::execution (schedulers, stop tokens), so moving to it once compilers
 * ship it is mechanical.
 *
 * Lifetimes, the part that matters: a task belongs to a TaskScope (owned by
 * whoever started it). When a task hops to a scheduler, its resumption
 * carries the scope's liveness: if the scope has gone by then, the task is
 * destroyed instead of resumed (it's suspended there, so that's safe), and
 * a resumption that's dropped without running (a queue closed at shutdown)
 * destroys it too. So:
 *   - between hops, code on a worker touches only its own data (the rule
 *     of threads.hpp), never its owner's;
 *   - the owner's state is touched after hopping back to the main thread,
 *     where a task only ever runs while its scope is alive.
 * A scope's stop token (stop_requested()) asks running tasks to finish
 * early; destroying the scope requests it.
 *
 * Tasks mustn't throw (an exception escaping one ends the compositor).
 * Pure C++20; tested under ThreadSanitizer (test/async_test.cpp).
 */
#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

#include "waybox/threads.hpp"

namespace wb {

/* Somewhere work runs. post() may be called from any thread. */
class Scheduler {
public:
	virtual ~Scheduler() = default;
	virtual void post(std::function<void()> work) = 0;
	/* After `delay`; schedulers without timers run it at once. */
	virtual void post_after(std::chrono::milliseconds delay, std::function<void()> work) {
		(void)delay;
		post(std::move(work));
	}
};

/* The existing queue and pool, as schedulers. */
class MainScheduler : public Scheduler {
public:
	explicit MainScheduler(std::shared_ptr<MainQueue> queue) : queue_(std::move(queue)) {}
	void post(std::function<void()> work) override { queue_->post(std::move(work)); }
	/* Timers come from whoever runs the loop (the compositor's event loop). */
	std::function<void(std::chrono::milliseconds, std::function<void()>)> timer;
	void post_after(std::chrono::milliseconds d, std::function<void()> work) override {
		if (timer)
			timer(d, std::move(work));
		else
			post(std::move(work));
	}

private:
	std::shared_ptr<MainQueue> queue_;
};

class WorkerScheduler : public Scheduler {
public:
	explicit WorkerScheduler(Workers &workers) : workers_(workers) {}
	void post(std::function<void()> work) override {
		workers_.submit([w = std::move(work)](std::stop_token) { w(); });
	}

private:
	Workers &workers_;
};

namespace detail {

/* A scope's liveness and stop request, shared with its tasks. */
struct ScopeState {
	std::atomic<bool> alive{true};
	std::stop_source stop;
	std::atomic<size_t> running{0};
};

struct PromiseBase {
	std::shared_ptr<ScopeState> scope;
	std::coroutine_handle<> root;          /* the task the scope owns */
	std::coroutine_handle<> continuation;  /* who awaits this one, if any */
	bool detached = false;                 /* a root: destroys itself when done */

	std::suspend_always initial_suspend() noexcept { return {}; }
	struct Final {
		bool await_ready() noexcept { return false; }
		template <typename P>
		std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
			PromiseBase &p = h.promise();
			if (p.continuation)
				return p.continuation;
			if (p.detached) {
				auto scope = p.scope;
				h.destroy();
				if (scope)
					--scope->running;
			}
			return std::noop_coroutine();
		}
		void await_resume() noexcept {}
	};
	Final final_suspend() noexcept { return {}; }
	void unhandled_exception() noexcept { std::terminate(); }
};

/* A posted resumption: resumes the task if its scope is alive, else (or if
 * never run) destroys the task's root. */
class Resumption {
public:
	Resumption(std::coroutine_handle<> h, std::coroutine_handle<> root,
			std::shared_ptr<ScopeState> scope)
		: h_(h), root_(root), scope_(std::move(scope)) {}
	~Resumption() { drop(); }
	Resumption(const Resumption &) = delete;
	Resumption &operator=(const Resumption &) = delete;
	void run() {
		std::coroutine_handle<> h = std::exchange(h_, nullptr);
		if (!h)
			return;
		if (scope_ != nullptr && !scope_->alive.load()) {
			h_ = h;
			drop();
			return;
		}
		h.resume();
	}

private:
	void drop() {
		if (!h_)
			return;
		h_ = nullptr;
		if (root_) {
			root_.destroy();  /* the whole chain: children live in its frame */
			root_ = nullptr;
			if (scope_)
				--scope_->running;
		}
	}
	std::coroutine_handle<> h_, root_;
	std::shared_ptr<ScopeState> scope_;
};

}  // namespace detail

template <typename T = void>
class Task;

namespace detail {

template <typename T>
struct Promise : PromiseBase {
	std::optional<T> value;
	Task<T> get_return_object();
	template <typename U>
	void return_value(U &&v) { value.emplace(std::forward<U>(v)); }
};

template <>
struct Promise<void> : PromiseBase {
	Task<void> get_return_object();
	void return_void() {}
};

}  // namespace detail

/* A lazy coroutine: runs when awaited, or when a TaskScope spawns it. */
template <typename T>
class [[nodiscard]] Task {
public:
	using promise_type = detail::Promise<T>;
	using Handle = std::coroutine_handle<promise_type>;

	explicit Task(Handle h) : h_(h) {}
	Task(Task &&o) noexcept : h_(std::exchange(o.h_, nullptr)) {}
	Task &operator=(Task &&o) noexcept {
		if (this != &o) {
			if (h_)
				h_.destroy();
			h_ = std::exchange(o.h_, nullptr);
		}
		return *this;
	}
	Task(const Task &) = delete;
	~Task() {
		if (h_)
			h_.destroy();
	}

	/* Awaited by another task: runs now, with the awaiting task's scope;
	 * the awaiting task continues when it's done. */
	bool await_ready() const noexcept { return false; }
	template <typename P>
	std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent) noexcept {
		detail::PromiseBase &pp = parent.promise();
		detail::PromiseBase &cp = h_.promise();
		cp.scope = pp.scope;
		cp.root = pp.root;
		cp.continuation = parent;
		return h_;
	}
	T await_resume() {
		if constexpr (!std::is_void_v<T>)
			return std::move(*h_.promise().value);
	}

	/* For TaskScope: the frame changes hands. */
	Handle release() { return std::exchange(h_, nullptr); }

private:
	Handle h_;
};

namespace detail {
template <typename T>
Task<T> Promise<T>::get_return_object() {
	return Task<T>(std::coroutine_handle<Promise<T>>::from_promise(*this));
}
inline Task<void> Promise<void>::get_return_object() {
	return Task<void>(std::coroutine_handle<Promise<void>>::from_promise(*this));
}
}  // namespace detail

/* Owns the tasks it spawns: started at once (on the calling thread, until
 * their first hop); destroyed with the scope as their resumptions come up.
 * Destroying the scope requests their stop. On the thread that owns it. */
class TaskScope {
public:
	TaskScope() : state_(std::make_shared<detail::ScopeState>()) {}
	~TaskScope() {
		state_->alive = false;
		state_->stop.request_stop();
	}
	TaskScope(const TaskScope &) = delete;
	TaskScope &operator=(const TaskScope &) = delete;

	template <typename T>
	void spawn(Task<T> task) {
		auto h = task.release();
		auto &p = h.promise();
		p.scope = state_;
		p.root = h;
		p.detached = true;
		++state_->running;
		h.resume();
	}
	std::stop_token stop_token() const { return state_->stop.get_token(); }
	void request_stop() { state_->stop.request_stop(); }
	/* Tasks not finished (spawned, not yet done or dropped). */
	size_t running() const { return state_->running.load(); }

private:
	std::shared_ptr<detail::ScopeState> state_;
};

/* co_await on(sched): continue on `sched`. */
struct OnScheduler {
	Scheduler &sched;
	std::chrono::milliseconds delay{0};
	bool await_ready() const noexcept { return false; }
	template <typename P>
	void await_suspend(std::coroutine_handle<P> h) {
		detail::PromiseBase &p = h.promise();
		auto r = std::make_shared<detail::Resumption>(h, p.root, p.scope);
		auto work = [r] { r->run(); };
		if (delay.count() > 0)
			sched.post_after(delay, std::move(work));
		else
			sched.post(std::move(work));
	}
	void await_resume() const noexcept {}
};
inline OnScheduler on(Scheduler &sched) { return {sched}; }
/* co_await sleep_for(sched, 100ms): continue on `sched`, at least that
 * much later. */
inline OnScheduler sleep_for(Scheduler &sched, std::chrono::milliseconds d) { return {sched, d}; }

/* co_await stop_requested(): whether the task's scope asked it to stop. */
struct StopRequested {
	bool stopped = false;
	bool await_ready() const noexcept { return false; }
	template <typename P>
	bool await_suspend(std::coroutine_handle<P> h) noexcept {
		auto &scope = h.promise().scope;
		stopped = scope != nullptr && scope->stop.stop_requested();
		return false;  /* no suspension: just a look */
	}
	bool await_resume() const noexcept { return stopped; }
};
inline StopRequested stop_requested() { return {}; }

}  // namespace wb

#endif /* WB_ASYNC_HPP */
