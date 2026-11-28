#ifndef WB_LOCATION_AGENT_HPP
#define WB_LOCATION_AGENT_HPP

/*
 * The location agent's decisions, without GTK or D-Bus (pure; test/
 * location_agent_test.cpp): GeoClue's accuracy levels, how near each is in
 * plain words, and the answers kept, in the xdg permission store's
 * "location" table as xdg-desktop-portal keeps them (an app's entry: its
 * accuracy's name, then when it was last allowed), so the two agree.
 */
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wb::location {

/* GeoClue's GClueAccuracyLevel. */
enum Level : uint32_t {
	kNone = 0,
	kCountry = 1,
	kCity = 4,
	kNeighborhood = 5,
	kStreet = 6,
	kExact = 8,
};

/* The permission store's names ("CITY", ...), as the portal writes them. */
std::string level_name(uint32_t level);
std::optional<uint32_t> level_from_name(const std::string &name);
/* How near, for the card: "to within a country", "to within a city",
 * "to within a neighborhood", "to within a street", "exactly". A level
 * between two named ones counts as the coarser. */
std::string nearness(uint32_t level);

/* How long an answer holds. */
enum class Hold { Once, Session, Always };

struct Answer {
	bool allow = false;
	uint32_t level = kNone;  /* what's given (allowed: what was asked; else none) */
};

/* A kept answer for `app`: the session's (this run of the agent) first,
 * then the store's (its entry: {name, last used}). nullopt: ask. An
 * allowed one gives at most what it allowed, and at most what's asked. */
std::optional<Answer> kept_answer(const std::string &app, uint32_t asked,
		const std::map<std::string, Answer> &session,
		const std::map<std::string, std::vector<std::string>> &store);

/* The store's entry for an answer kept always: {name, seconds}. */
std::vector<std::string> store_entry(const Answer &a, int64_t now_seconds);

}  // namespace wb::location

#endif /* WB_LOCATION_AGENT_HPP */
