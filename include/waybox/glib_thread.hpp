#ifndef WB_GLIB_THREAD_HPP
#define WB_GLIB_THREAD_HPP

/*
 * The one thread in Waybox that runs GLib's loop: everything that needs
 * GLib or GIO (D-Bus above all: the agent hub's interface) runs here, as
 * work posted to it or tasks hopping onto it (async.hpp), never on a thread
 * of its own. Its context is the thread-default one for the thread's whole
 * life, so GIO's async calls made here complete here.
 *
 * Work still queued when it's destroyed is dropped (a task waiting on it is
 * destroyed, as async.hpp says).
 */
#include <glib.h>

#include <chrono>
#include <functional>
#include <thread>

#include "waybox/async.hpp"

namespace wb {

class GLibThread : public Scheduler {
public:
	GLibThread();
	~GLibThread() override;
	GLibThread(const GLibThread &) = delete;
	GLibThread &operator=(const GLibThread &) = delete;

	GMainContext *context() const { return ctx_; }
	void post(std::function<void()> work) override;
	void post_after(std::chrono::milliseconds delay, std::function<void()> work) override;

private:
	GMainContext *ctx_ = nullptr;
	GMainLoop *loop_ = nullptr;
	std::jthread thread_;
};

}  // namespace wb

#endif /* WB_GLIB_THREAD_HPP */
