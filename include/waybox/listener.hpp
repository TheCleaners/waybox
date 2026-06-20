#ifndef WB_LISTENER_HPP
#define WB_LISTENER_HPP

#include <functional>

#include "waybox/wlroots.hpp"

namespace wb {

/*
 * RAII wrapper around a wl_listener.
 *
 * Connect it to a wl_signal with any callable (typically a lambda that
 * captures the context it needs); it automatically removes itself from the
 * signal when it is reconnected or destroyed, so the manual wl_list_remove()
 * teardown that is so easy to get wrong (use-after-free, double-remove, leaks)
 * disappears.
 *
 * Non-copyable and non-movable: the embedded wl_listener.link is stored by
 * address inside an intrusive wl_list, so the object must not change address
 * while connected. Hold one as a data member of a heap-allocated object, or in
 * a std::unique_ptr.
 */
class Listener {
public:
	using Callback = std::function<void(void *data)>;

	Listener() {
		m_wrap.owner = this;
		m_wrap.listener.notify = &Listener::handle;
		wl_list_init(&m_wrap.listener.link);
	}

	~Listener() { disconnect(); }

	Listener(const Listener &) = delete;
	Listener &operator=(const Listener &) = delete;
	Listener(Listener &&) = delete;
	Listener &operator=(Listener &&) = delete;

	/* Connect to a signal, replacing any existing connection/callback. */
	void connect(struct wl_signal *signal, Callback callback) {
		disconnect();
		m_callback = std::move(callback);
		wl_signal_add(signal, &m_wrap.listener);
	}

	void disconnect() {
		if (!wl_list_empty(&m_wrap.listener.link)) {
			wl_list_remove(&m_wrap.listener.link);
			wl_list_init(&m_wrap.listener.link);
		}
	}

	[[nodiscard]] bool connected() const {
		return !wl_list_empty(&m_wrap.listener.link);
	}

private:
	/* Standard-layout helper so wl_container_of()'s offsetof is well defined
	 * even though Listener itself is not standard-layout. */
	struct Wrapper {
		struct wl_listener listener;
		Listener *owner;
	};

	static void handle(struct wl_listener *listener, void *data) {
		Wrapper *wrap = wl_container_of(listener, wrap, listener);
		if (wrap->owner->m_callback) {
			wrap->owner->m_callback(data);
		}
	}

	Wrapper m_wrap;
	Callback m_callback;
};

}  // namespace wb

#endif /* WB_LISTENER_HPP */
