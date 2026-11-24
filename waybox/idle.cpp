#include "waybox/wlroots.hpp"

#include "idle.h"

namespace {
/* One per inhibitor, so each one's going away is seen (a single shared
 * listener only tracked the newest, and idle could stay blocked). */
struct Inhibitor {
	wb::Listener destroy;
};
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

bool create_idle_manager(struct wb_server *server) {
	server->idle_notifier = wlr_idle_notifier_v1_create(server->wl_display);
	server->idle_inhibit_manager = wlr_idle_inhibit_v1_create(server->wl_display);

	server->new_inhibitor.connect(&server->idle_inhibit_manager->events.new_inhibitor,
			[server](void *data) { idle_inhibitor_new(server, data); });
	server->destroy_inhibit_manager.connect(&server->idle_inhibit_manager->events.destroy,
			[server](void *) { server->new_inhibitor.disconnect(); });
	return true;
}
