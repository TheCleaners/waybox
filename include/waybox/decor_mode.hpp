#ifndef WB_DECOR_MODE_HPP
#define WB_DECOR_MODE_HPP

namespace wb {

/*
 * Per-window decoration mode (pure model).
 *
 * Openbox lets windows be decorated or not, both per-application (rc.xml
 * <applications><application><decor>) and at runtime (the ToggleDecorations
 * action). Wayland negotiates server- vs client-side decorations per surface
 * via xdg-decoration, so the user's preference plus the client's capability
 * decide the outcome. The decision is pure and unit-tested; applying the chosen
 * mode to the live wlr_xdg_toplevel_decoration lives in decoration.cpp.
 */

enum class DecorMode {
	Default,  /* follow the client's preference / compositor default */
	Full,     /* decorated: server-side frame (titlebar + border) */
	None,     /* undecorated: no server frame */
};

/* The negotiated outcome we hand to xdg-decoration. */
enum class NegotiatedDecoration {
	ServerSide,  /* waybox draws the frame (SSD) */
	ClientSide,  /* the client draws its own decorations, if any (CSD) */
};

/*
 * Resolve the decoration to use for a window.
 *
 * `pref` is the per-window preference (from an app rule or a runtime toggle);
 * `client_prefers_server_side` is what the client asked for via xdg-decoration;
 * `ssd_available` is whether waybox can actually draw server decorations yet
 * (false until the SSD renderer exists). When SSD is unavailable we never claim
 * ServerSide — that would leave a window with no frame at all — so the result
 * always degrades safely to ClientSide.
 */
NegotiatedDecoration negotiate_decoration(DecorMode pref,
		bool client_prefers_server_side, bool ssd_available);

/*
 * Flip a window's decoration preference for ToggleDecorations: a decorated
 * window (Full or Default) becomes None, an undecorated one becomes Full.
 */
DecorMode toggle_decor(DecorMode current);

}  // namespace wb

#endif /* WB_DECOR_MODE_HPP */
