/*
 * The hub's tunables (tunables.hpp).
 */
#include "tunables.hpp"

#include <cctype>
#include <cstdint>
#include <optional>

#include "waybox/ini_doc.hpp"

namespace wb::agent {

namespace {

enum class Kind { Size, Duration, Count };

/* "64 KiB", "2 s", "8": the value in bytes, milliseconds or as is. */
std::optional<uint64_t> parse(std::string_view v, Kind kind) {
	size_t i = 0;
	while (i < v.size() && isspace(static_cast<unsigned char>(v[i])))
		++i;
	uint64_t n = 0;
	size_t digits = 0;
	while (i < v.size() && isdigit(static_cast<unsigned char>(v[i]))) {
		if (n > (UINT64_MAX - 9) / 10)
			return std::nullopt;
		n = n * 10 + static_cast<uint64_t>(v[i] - '0');
		++i;
		++digits;
	}
	if (digits == 0)
		return std::nullopt;
	while (i < v.size() && isspace(static_cast<unsigned char>(v[i])))
		++i;
	std::string_view unit = v.substr(i);
	while (!unit.empty() && isspace(static_cast<unsigned char>(unit.back())))
		unit.remove_suffix(1);
	uint64_t mult = 0;
	switch (kind) {
	case Kind::Size:
		mult = unit.empty() || unit == "B" ? 1 : unit == "KiB" ? 1024 : unit == "MiB" ? 1024 * 1024 : 0;
		break;
	case Kind::Duration:
		mult = unit == "ms" ? 1 : unit == "s" ? 1000 : unit == "min" ? 60000 : 0;
		break;
	case Kind::Count:
		mult = unit.empty() ? 1 : 0;
		break;
	}
	if (mult == 0 || n > UINT64_MAX / mult)
		return std::nullopt;
	return n * mult;
}

}  // namespace

void apply_tunables(Tunables &t, std::string_view ini_text, bool system,
		std::vector<std::string> &notes) {
	auto doc = ini::Document::parse(ini_text);
	struct Key {
		const char *name;
		Kind kind;
		uint64_t least;  /* a value below this would stop the hub working */
		uint64_t *value;
	};
	uint64_t frame = t.max_frame, queued = t.max_queued, agents = t.max_agents,
			listens = t.max_listens, wait = static_cast<uint64_t>(t.handshake_wait.count());
	Key keys[] = {
		{"MaxFrame", Kind::Size, 4096, &frame},
		{"MaxQueued", Kind::Size, 4096, &queued},
		{"MaxAgents", Kind::Count, 1, &agents},
		{"MaxListens", Kind::Count, 0, &listens},
		{"HandshakeWait", Kind::Duration, 100, &wait},
	};
	for (const Key &k : keys) {
		auto raw = doc.get("Tunables", k.name);
		if (!raw)
			continue;
		auto v = parse(*raw, k.kind);
		if (!v || *v < k.least) {
			notes.push_back(std::string(k.name) + "=" + *raw + ": not a value it can take; kept " +
					std::to_string(*k.value));
			continue;
		}
		/* Each of these guards by being low: higher weakens it. */
		if (!system && *v > *k.value) {
			notes.push_back(std::string(k.name) + "=" + *raw +
					": raising a guard needs the compositor's confirmation (not yet); kept " +
					std::to_string(*k.value));
			continue;
		}
		*k.value = *v;
	}
	t.max_frame = frame;
	t.max_queued = queued;
	t.max_agents = agents;
	t.max_listens = listens;
	t.handshake_wait = std::chrono::milliseconds(wait);
}

}  // namespace wb::agent
