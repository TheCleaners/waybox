#include "wb_test.hpp"

#include "../agent/tunables.hpp"

using wb::agent::apply_tunables;
using wb::agent::Tunables;

WB_TEST(tunables_suggested) {
	Tunables t;
	WB_CHECK(t.max_frame == 4u * 1024 * 1024 && t.max_queued == 8u * 1024 * 1024);
	WB_CHECK(t.max_agents == 32 && t.max_listens == 8 && t.handshake_wait.count() == 10000);
}

WB_TEST(tunables_units) {
	Tunables t;
	std::vector<std::string> notes;
	apply_tunables(t, "[Tunables]\nMaxQueued=64 KiB\nMaxFrame=1MiB\nHandshakeWait=3 s\n"
			"MaxAgents=4\nMaxListens=2\n", true, notes);
	WB_CHECK(notes.empty());
	WB_CHECK(t.max_queued == 64 * 1024 && t.max_frame == 1024 * 1024);
	WB_CHECK(t.handshake_wait.count() == 3000 && t.max_agents == 4 && t.max_listens == 2);
	apply_tunables(t, "[Tunables]\nHandshakeWait=250ms\nMaxQueued=8192\n", true, notes);
	WB_CHECK(t.handshake_wait.count() == 250 && t.max_queued == 8192);
}

WB_TEST(tunables_refused) {
	Tunables t;
	std::vector<std::string> notes;
	/* Not values it can take: a unit of another kind, none where one's
	 * needed, too small to work, nonsense. */
	apply_tunables(t, "[Tunables]\nMaxQueued=64 s\nHandshakeWait=3\nMaxFrame=10\n"
			"MaxAgents=lots\nMaxListens=-1\n", true, notes);
	WB_CHECK(notes.size() == 5);
	Tunables d;
	WB_CHECK(t.max_queued == d.max_queued && t.handshake_wait == d.handshake_wait);
	WB_CHECK(t.max_frame == d.max_frame && t.max_agents == d.max_agents);
	/* Other sections don't count. */
	notes.clear();
	apply_tunables(t, "[Agent Tools]\nMaxAgents=2\n", true, notes);
	WB_CHECK(t.max_agents == 32 && notes.empty());
}

WB_TEST(tunables_user_tightens_only) {
	Tunables t;
	std::vector<std::string> notes;
	apply_tunables(t, "[Tunables]\nMaxAgents=4\nMaxQueued=64 MiB\nHandshakeWait=1 min\n", false, notes);
	WB_CHECK(t.max_agents == 4);
	/* Raising a guard: not from the user's file (yet). */
	WB_CHECK(t.max_queued == 8u * 1024 * 1024 && t.handshake_wait.count() == 10000);
	WB_CHECK(notes.size() == 2 && notes[0].find("confirmation") != std::string::npos);
	/* The system's may raise one. */
	notes.clear();
	apply_tunables(t, "[Tunables]\nMaxQueued=64 MiB\n", true, notes);
	WB_CHECK(t.max_queued == 64u * 1024 * 1024 && notes.empty());
}
