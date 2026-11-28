/*
 * The shared GLib thread (glib_thread.hpp).
 */
#include "waybox/glib_thread.hpp"

#include <latch>

namespace wb {

namespace {

gboolean run_once(gpointer data) {
	(*static_cast<std::function<void()> *>(data))();
	return G_SOURCE_REMOVE;
}

void free_work(gpointer data) {
	delete static_cast<std::function<void()> *>(data);
}

}  // namespace

GLibThread::GLibThread() {
	ctx_ = g_main_context_new();
	loop_ = g_main_loop_new(ctx_, FALSE);
	std::latch started(1);
	thread_ = std::jthread([this, &started] {
		block_signals();
		g_main_context_push_thread_default(ctx_);
		/* Running before the constructor returns: posts can't race the
		 * loop's start. */
		g_main_context_invoke(ctx_, [](gpointer l) -> gboolean {
			static_cast<std::latch *>(l)->count_down();
			return G_SOURCE_REMOVE;
		}, &started);
		g_main_loop_run(loop_);
		g_main_context_pop_thread_default(ctx_);
	});
	started.wait();
}

GLibThread::~GLibThread() {
	g_main_loop_quit(loop_);  /* thread-safe; wakes the context */
	thread_.join();
	g_main_loop_unref(loop_);
	/* Unreffing the context destroys its remaining sources: their work is
	 * freed (free_work), not run. */
	g_main_context_unref(ctx_);
}

void GLibThread::post(std::function<void()> work) {
	GSource *s = g_idle_source_new();
	g_source_set_priority(s, G_PRIORITY_DEFAULT);
	g_source_set_callback(s, run_once, new std::function<void()>(std::move(work)), free_work);
	g_source_attach(s, ctx_);
	g_source_unref(s);
}

void GLibThread::post_after(std::chrono::milliseconds delay, std::function<void()> work) {
	GSource *s = g_timeout_source_new(static_cast<guint>(delay.count()));
	g_source_set_callback(s, run_once, new std::function<void()>(std::move(work)), free_work);
	g_source_attach(s, ctx_);
	g_source_unref(s);
}

}  // namespace wb
