#ifndef WB_SESSION_STORE_HPP
#define WB_SESSION_STORE_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "waybox/geometry.hpp"

namespace wb {

/*
 * What session restore (xdg-session-management) keeps: per session id, its
 * windows by the names the app gave them. Pure and unit-tested; the
 * protocol (xdg_session.cpp) fills it and saves it to
 * $XDG_STATE_HOME/waybox/sessions.json.
 */

struct SavedWindow {
	/* The window's size, and its position relative to the output it was
	 * on (so it comes back on that output, wherever it now is). For a
	 * maximized or fullscreen window, the geometry it returns to. */
	int x = 0, y = 0, width = 0, height = 0;
	std::string output;
	bool maximized = false, fullscreen = false;
	int desktop = 0;  /* 0-based */
	bool omnipresent = false;
	bool operator==(const SavedWindow &) const = default;
};

struct SavedSession {
	std::map<std::string, SavedWindow> windows;  /* by the app's name for it */
	int64_t used = 0;  /* last in use, Unix seconds */
	bool operator==(const SavedSession &) const = default;
};

class SessionStore {
public:
	static constexpr size_t kMaxSessions = 200;
	static constexpr size_t kMaxWindows = 100;    /* per session */
	static constexpr size_t kMaxNameBytes = 1024;  /* ids and names (the protocol allows 4K) */
	static constexpr int64_t kMaxAgeSeconds = 90LL * 24 * 3600;
	/* The file read at most (200 sessions of 100 windows fit well within). */
	static constexpr size_t kMaxFileBytes = 8 * 1024 * 1024;
	/* A timestamp beyond this (about year 33,000) isn't one: well inside
	 * int64_t and a double's exact integers. */
	static constexpr int64_t kMaxTimestamp = 1'000'000'000'000LL;
	/* The format written; one without a version is this one too. */
	static constexpr int kVersion = 1;

	std::map<std::string, SavedSession> sessions;

	std::string to_json() const;
	/* Replaces what's here, only if the text is a store (else false, and
	 * what was here stays): of this version (or none), not longer than
	 * kMaxFileBytes. `newer`: set when it's a later version's, which this
	 * Waybox mustn't overwrite. A timestamp that isn't a whole number in
	 * [0, kMaxTimestamp] counts as long unused (it expires). */
	bool from_json(std::string_view text, bool *newer = nullptr);

	/* Old sessions go: unused for kMaxAgeSeconds, then the least recently
	 * used beyond kMaxSessions. */
	void expire(int64_t now);

	/* Set a window's state, within the limits (a name too long, or one
	 * window too many, isn't kept). False if not kept. */
	bool set_window(const std::string &session, const std::string &name, const SavedWindow &w);
};

/* An output as it is now, to place a restored window on. */
struct OutputBox {
	std::string name;
	Rect box;  /* the area windows go in (usable area) */
};

/* Where a saved window goes now: on its output if that's still there
 * (moved into it if it no longer fits), else nullopt (placed as a new
 * window, at its saved size). The size never exceeds the output's. */
std::optional<Rect> restored_rect(const SavedWindow &w, const std::vector<OutputBox> &outputs);

/* A new session id: 32 random hex digits. */
std::string new_session_id();

/* Valid UTF-8 (the protocol's strings must be), without NUL. */
bool valid_utf8(std::string_view s);

}  // namespace wb

#endif
