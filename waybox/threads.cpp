/*
 * The main thread's mailbox and the worker pool (threads.hpp).
 */
#include "waybox/threads.hpp"

#include <algorithm>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace wb {

MainQueue::MainQueue() {
	fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (fd_ < 0) {
		perror("waybox: eventfd");
		std::abort();
	}
}

MainQueue::~MainQueue() {
	close();
	::close(fd_);
}

void MainQueue::post(std::function<void()> work) {
	{
		std::lock_guard lock(mutex_);
		if (closed_)
			return;
		waiting_.push_back(std::move(work));
	}
	uint64_t one = 1;
	/* Only fails if the counter would overflow: it's readable anyway. */
	(void)!write(fd_, &one, sizeof one);
}

size_t MainQueue::run() {
	uint64_t n = 0;
	(void)!read(fd_, &n, sizeof n);  /* readable no more, until a post */
	std::deque<std::function<void()>> now;
	{
		std::lock_guard lock(mutex_);
		now.swap(waiting_);
	}
	for (auto &work : now)
		work();
	return now.size();
}

void MainQueue::close() {
	std::deque<std::function<void()>> dropped;
	{
		std::lock_guard lock(mutex_);
		closed_ = true;
		dropped.swap(waiting_);
	}
	/* (Destroyed outside the lock: a job's captures may post nothing, but
	 * may be heavy to free.) */
}

Workers::Workers(unsigned count) {
	count = std::max(1u, count);
	threads_.reserve(count);
	for (unsigned i = 0; i < count; ++i)
		threads_.emplace_back([this](std::stop_token stop) { run(stop); });
}

Workers::~Workers() {
	for (auto &t : threads_)
		t.request_stop();
	ready_.notify_all();
	threads_.clear();  /* joins */
}

void Workers::submit(Job job) {
	{
		std::lock_guard lock(mutex_);
		if (threads_.empty() || threads_.front().get_stop_token().stop_requested())
			return;
		jobs_.push_back(std::move(job));
	}
	ready_.notify_one();
}

unsigned Workers::default_count() {
	unsigned hw = std::thread::hardware_concurrency();
	return std::clamp(hw / 2, 1u, 4u);
}

void block_signals() {
	sigset_t all;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, nullptr);
}

void Workers::run(std::stop_token stop) {
	block_signals();
	while (true) {
		Job job;
		{
			std::unique_lock lock(mutex_);
			/* (wait returns true when there's a job even if stopping:
			 * stopping wins, and jobs not started are dropped.) */
			if (!ready_.wait(lock, stop, [this] { return !jobs_.empty(); }) ||
					stop.stop_requested())
				return;
			job = std::move(jobs_.front());
			jobs_.pop_front();
		}
		job(stop);
	}
}

}  // namespace wb
