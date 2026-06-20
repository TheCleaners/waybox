/*
 * Pure per-window decoration-mode logic (no wlroots). Unit-tested.
 */
#include "waybox/decor_mode.hpp"

namespace wb {

NegotiatedDecoration negotiate_decoration(DecorMode pref,
		bool client_prefers_server_side, bool ssd_available) {
	switch (pref) {
	case DecorMode::None:
		return NegotiatedDecoration::ClientSide;
	case DecorMode::Full:
		/* Only honour SSD when waybox can actually draw it; otherwise a
		 * "decorated" window would end up with no frame at all. */
		return ssd_available ? NegotiatedDecoration::ServerSide
				: NegotiatedDecoration::ClientSide;
	case DecorMode::Default:
		break;
	}
	if (ssd_available && client_prefers_server_side)
		return NegotiatedDecoration::ServerSide;
	return NegotiatedDecoration::ClientSide;
}

DecorMode toggle_decor(DecorMode current) {
	return current == DecorMode::None ? DecorMode::Full : DecorMode::None;
}

}  // namespace wb
