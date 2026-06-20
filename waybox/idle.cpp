#include "waybox/wlroots.hpp"

#include "idle.h"

static void idle_inhibitor_new(struct wb_server *server, void *data) {
	auto *inhibitor = static_cast<struct wlr_idle_inhibitor_v1 *>(data);

	server->destroy_inhibitor.connect(&inhibitor->events.destroy,
			[server](void *data) {
		/* wlroots destroys the inhibitor after this callback, so the count is
		 * still 1 when the last inhibitor is being destroyed. */
		server->destroy_inhibitor.disconnect();
		wlr_idle_notifier_v1_set_inhibited(server->idle_notifier,
				wl_list_length(&server->inhibitors) > 1);
	});
	wl_list_remove(&inhibitor->link);
	wl_list_insert(&server->inhibitors, &inhibitor->link);
	wlr_idle_notifier_v1_set_inhibited(server->idle_notifier, true);
}

bool create_idle_manager(struct wb_server *server) {
	server->idle_notifier = wlr_idle_notifier_v1_create(server->wl_display);
	server->idle_inhibit_manager = wlr_idle_inhibit_v1_create(server->wl_display);

	wl_list_init(&server->inhibitors);
	server->new_inhibitor.connect(&server->idle_inhibit_manager->events.new_inhibitor,
			[server](void *data) { idle_inhibitor_new(server, data); });
	server->destroy_inhibit_manager.connect(&server->idle_inhibit_manager->events.destroy,
			[server](void *data) {
		server->new_inhibitor.disconnect();
		wl_list_remove(&server->inhibitors);
	});
	return true;
}
