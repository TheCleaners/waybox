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
			listens = t.max_listens, wait = static_cast<uint64_t>(t.handshake_wait.count()),
			timeout = static_cast<uint64_t>(t.call_timeout.count()),
			deadline = static_cast<uint64_t>(t.call_deadline.count()),
			in_flight = t.max_in_flight, rate = t.call_rate;
	Key keys[] = {
		{"MaxFrame", Kind::Size, 4096, &frame},
		{"MaxQueued", Kind::Size, 4096, &queued},
		{"MaxAgents", Kind::Count, 1, &agents},
		{"MaxListens", Kind::Count, 0, &listens},
		{"HandshakeWait", Kind::Duration, 100, &wait},
		{"CallTimeout", Kind::Duration, 100, &timeout},
		{"CallDeadline", Kind::Duration, 100, &deadline},
		{"MaxInFlight", Kind::Count, 1, &in_flight},
		{"CallRate", Kind::Count, 1, &rate},
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
	t.call_timeout = std::chrono::milliseconds(timeout);
	t.call_deadline = std::chrono::milliseconds(deadline);
	t.max_in_flight = in_flight;
	t.call_rate = rate;
	/* The audit's: from a distribution's file only. */
	uint64_t cut = t.audit_cut, rotate = t.audit_rotate, keep = t.audit_keep;
	Key audit[] = {
		{"AuditCut", Kind::Size, 256, &cut},
		{"AuditKeep", Kind::Count, 1, &keep},
	};
	for (const Key &k : audit) {
		auto raw = doc.get("Tunables", k.name);
		if (!raw)
			continue;
		auto v = parse(*raw, k.kind);
		if (!system)
			notes.push_back(std::string(k.name) + ": the audit's tunables are a distribution's to set");
		else if (!v || *v < k.least)
			notes.push_back(std::string(k.name) + "=" + *raw + ": not a value it can take");
		else
			*k.value = *v;
	}
	/* AuditRotate: a size, and optionally how many are kept ("10 MiB, 3 kept"). */
	if (auto raw = doc.get("Tunables", "AuditRotate")) {
		std::string_view r = *raw;
		size_t comma = r.find(',');
		auto size = parse(r.substr(0, comma), Kind::Size);
		std::optional<uint64_t> kept;
		if (comma != std::string_view::npos) {
			std::string_view k = r.substr(comma + 1);
			size_t word = k.find("kept");
			kept = parse(word == std::string_view::npos ? k : k.substr(0, word), Kind::Count);
		}
		if (!system)
			notes.push_back("AuditRotate: the audit's tunables are a distribution's to set");
		else if (!size || *size < 4096 || (comma != std::string_view::npos && (!kept || *kept < 1)))
			notes.push_back("AuditRotate=" + *raw + ": not a value it can take");
		else {
			rotate = *size;
			if (kept)
				keep = *kept;
		}
	}
	if (auto raw = doc.get("Tunables", "AuditSync")) {
		if (!system)
			notes.push_back("AuditSync: the audit's tunables are a distribution's to set");
		else if (*raw == "important" || *raw == "all" || *raw == "none")
			t.audit_sync = *raw;
		else
			notes.push_back("AuditSync=" + *raw + ": important, all or none");
	}
	t.audit_cut = cut;
	t.audit_rotate = rotate;
	t.audit_keep = keep;
	t.max_frame = frame;
	t.max_queued = queued;
	t.max_agents = agents;
	t.max_listens = listens;
	t.handshake_wait = std::chrono::milliseconds(wait);
}

}  // namespace wb::agent
