#ifndef WB_APP_RULE_XML_HPP
#define WB_APP_RULE_XML_HPP

/*
 * One <application> rule, read from and written to rc.xml: the one reader
 * Waybox's config and wbconf's editor share, so a rule means the same to
 * both. What Waybox doesn't read (Openbox's <shade>, <monitor>, role= and
 * the like, a position such as "center") is reported, and left exactly as
 * written when the rule is edited. (test/wbconf_test.cpp, app_rules_*.)
 */
#include <string>
#include <vector>

#include <libxml/tree.h>

#include "waybox/applications.hpp"

namespace wb {

struct AppRuleXml {
	AppRule rule;
	/* What's there that Waybox doesn't read, by name ("shade", "role",
	 * "monitor"), in the order met: kept as it is. */
	std::vector<std::string> kept;
	/* <position> holds more than plain <x>/<y> numbers ("center", 50%,
	 * force=, <monitor>): read as far as Waybox does, never rewritten. */
	bool position_kept = false;
};

AppRuleXml read_app_rule(xmlNode *app);

/* The rule's settings into `app`, in place: what it sets written (yes/no,
 * a desktop number or "all", above/below), what it leaves unset removed,
 * all else (comments, what Waybox doesn't read, a kept <position>) left as
 * it is. */
void write_app_rule(xmlNode *app, const AppRule &rule, bool position_kept);

}  // namespace wb

#endif /* WB_APP_RULE_XML_HPP */
