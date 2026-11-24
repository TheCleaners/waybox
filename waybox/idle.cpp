#include "waybox/wlroots.hpp"

#include <ctime>

#include "config.h"
#include "idle.h"
#include "waybox/output.h"

namespace {
/* One per inhibitor, so each one's going away is seen (a single shared
 * listener only tracked the newest, and idle could stay blocked). */
struct Inhibitor {
	wb::Listener destroy;
};

int64_t now_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

int screen_off_ms(struct wb_server *server) {
	return server->config != nullptr ? server->config->idle_screen_off_s * 1000 : 0;
}
}  // namespace

static void set_inhibited(struct wb_server *server, bool on) {
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, on);
	wlr_log(WLR_INFO, "wb-idle inhibited %d", on ? 1 : 0);
}

static void idle_inhibitor_new(struct wb_server *server, void *data) {
	auto *inhibitor = static_cast<struct wlr_idle_inhibitor_v1 *>(data);
	auto *wi = new Inhibitor;
	wi->destroy.connect(&inhibitor->events.destroy, [server, wi](void *) {
		/* wlroots unlinks the inhibitor after this callback, so it still
		 * counts itself. */
		set_inhibited(server,
				wl_list_length(&server->idle_inhibit_manager->inhibitors) > 1);
		delete wi;  /* the Listener disconnects itself */
	});
	set_inhibited(server, true);
}

/* <idle screenOff>: the timer fires at the deadline (it isn't re-armed on
 * every input event, only the time of the last one is noted), then either
 * switches the screens off or waits for the rest. */
static void arm(struct wb_server *server, int ms) {
	if (server->idle_timer != nullptr)
		wl_event_source_timer_update(server->idle_timer, ms);
}

static int idle_timeout(void *data) {
	auto *server = static_cast<struct wb_server *>(data);
	int timeout = screen_off_ms(server);
	if (timeout <= 0)
		return 0;
	int64_t left = timeout - (now_ms() - server->idle_last_activity_ms);
	if (left > 0) {
		arm(server, static_cast<int>(left));
		return 0;
	}
	/* A video player or game asked to keep the screen on. */
	if (!wl_list_empty(&server->idle_inhibit_manager->inhibitors)) {
		arm(server, timeout);
		return 0;
	}
	struct wb_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output_set_power(output, false))
			output->idle_off = true;
	}
	return 0;
}

void idle_activity(struct wb_server *server, struct wlr_seat *seat) {
	wlr_idle_notifier_v1_notify_activity(server->idle_notifier, seat);
	server->idle_last_activity_ms = now_ms();
	struct wb_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->idle_off)
			output_set_power(output, true);
	}
}

void idle_reconfigured(struct wb_server *server) {
	server->idle_last_activity_ms = now_ms();
	int timeout = screen_off_ms(server);
	arm(server, timeout > 0 ? timeout : 0);  /* 0 disarms */
}

bool create_idle_manager(struct wb_server *server) {
	server->idle_notifier = wlr_idle_notifier_v1_create(server->wl_display);
	server->idle_inhibit_manager = wlr_idle_inhibit_v1_create(server->wl_display);
	server->idle_timer = wl_event_loop_add_timer(server->wl_event_loop,
			idle_timeout, server);

	server->new_inhibitor.connect(&server->idle_inhibit_manager->events.new_inhibitor,
			[server](void *data) { idle_inhibitor_new(server, data); });
	server->destroy_inhibit_manager.connect(&server->idle_inhibit_manager->events.destroy,
			[server](void *) { server->new_inhibitor.disconnect(); });
	return true;
}

void finish_idle(struct wb_server *server) {
	if (server->idle_timer != nullptr) {
		wl_event_source_remove(server->idle_timer);
		server->idle_timer = nullptr;
	}
}
