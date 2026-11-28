#include "waybox/location_agent.hpp"


namespace wb::location {

std::string level_name(uint32_t level) {
	if (level >= kExact)
		return "EXACT";
	if (level >= kStreet)
		return "STREET";
	if (level >= kNeighborhood)
		return "NEIGHBORHOOD";
	if (level >= kCity)
		return "CITY";
	if (level >= kCountry)
		return "COUNTRY";
	return "NONE";
}

std::optional<uint32_t> level_from_name(const std::string &name) {
	static const std::map<std::string, uint32_t> kNames = {{"NONE", kNone},
			{"COUNTRY", kCountry}, {"CITY", kCity}, {"NEIGHBORHOOD", kNeighborhood},
			{"STREET", kStreet}, {"EXACT", kExact}};
	auto it = kNames.find(name);
	if (it == kNames.end())
		return std::nullopt;
	return it->second;
}

std::string nearness(uint32_t level) {
	if (level >= kExact)
		return "exactly";
	if (level >= kStreet)
		return "to within a street";
	if (level >= kNeighborhood)
		return "to within a neighborhood";
	if (level >= kCity)
		return "to within a city";
	return "to within a country";
}

std::optional<Answer> kept_answer(const std::string &app, uint32_t asked,
		const std::map<std::string, Answer> &session,
		const std::map<std::string, std::vector<std::string>> &store) {
	std::optional<Answer> kept;
	if (auto it = session.find(app); it != session.end()) {
		kept = it->second;
	} else if (auto st = store.find(app); st != store.end() && !st->second.empty()) {
		std::optional<uint32_t> level = level_from_name(st->second[0]);
		if (!level)
			return std::nullopt;  /* not one of ours to read: ask */
		kept = Answer{*level != kNone, *level};
	}
	if (!kept)
		return std::nullopt;
	if (!kept->allow)
		return Answer{false, kNone};
	/* Allowed up to what was allowed (asked for more: ask again). */
	if (asked > kept->level)
		return std::nullopt;
	return Answer{true, asked};
}

std::vector<std::string> store_entry(const Answer &a, int64_t now_seconds) {
	return {a.allow ? level_name(a.level) : "NONE", std::to_string(now_seconds)};
}

}  // namespace wb::location
