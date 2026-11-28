#include "wb_test.hpp"

#include "waybox/location_agent.hpp"

using namespace wb::location;

/* The store's names, both ways; a level between two names is the coarser. */
WB_TEST(location_level_names) {
	WB_CHECK(level_name(kCity) == "CITY" && level_name(kExact) == "EXACT");
	WB_CHECK(level_name(2) == "COUNTRY" && level_name(7) == "STREET");
	WB_CHECK(level_from_name("NEIGHBORHOOD") == kNeighborhood);
	WB_CHECK(!level_from_name("city").has_value());
	WB_CHECK(nearness(kCity) == "to within a city");
	WB_CHECK(nearness(kExact) == "exactly");
	WB_CHECK(nearness(kCountry) == "to within a country");
}

/* A kept answer: the session's first, then the store's; a denial denies;
 * an allowance covers what it allowed, not more. */
WB_TEST(location_kept_answers) {
	std::map<std::string, Answer> session;
	std::map<std::string, std::vector<std::string>> store;
	WB_CHECK(!kept_answer("maps", kCity, session, store));  /* nothing kept: ask */
	store["maps"] = {"CITY", "1700000000"};
	auto a = kept_answer("maps", kCity, session, store);
	WB_CHECK(a && a->allow && a->level == kCity);
	WB_CHECK(!kept_answer("maps", kExact, session, store));  /* more than allowed */
	a = kept_answer("maps", kCountry, session, store);
	WB_CHECK(a && a->allow && a->level == kCountry);  /* less: given as asked */
	store["spy"] = {"NONE", "1700000000"};
	a = kept_answer("spy", kCountry, session, store);
	WB_CHECK(a && !a->allow && a->level == kNone);
	/* The session's answer goes first. */
	session["spy"] = Answer{true, kExact};
	a = kept_answer("spy", kStreet, session, store);
	WB_CHECK(a && a->allow && a->level == kStreet);
	/* An entry it can't read: ask. */
	store["odd"] = {"SOMETIMES"};
	WB_CHECK(!kept_answer("odd", kCity, {}, store));
	auto e = store_entry(Answer{true, kNeighborhood}, 42);
	WB_CHECK(e.size() == 2 && e[0] == "NEIGHBORHOOD" && e[1] == "42");
	WB_CHECK(store_entry(Answer{false, kNone}, 1)[0] == "NONE");
}
