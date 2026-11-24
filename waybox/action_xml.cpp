/*
 * <action> element parsing shared by rc.xml and menu.xml (libxml).
 */
#include "waybox/action_xml.hpp"

#include <cstring>
#include <string>

namespace wb {

namespace {

const char *attr(xmlNode *node, const char *name) {
	for (xmlAttr *a = node->properties; a; a = a->next) {
		if (strcmp(reinterpret_cast<const char *>(a->name), name) == 0 &&
				a->children && a->children->content)
			return reinterpret_cast<const char *>(a->children->content);
	}
	return "";
}

bool is(xmlNode *node, const char *name) {
	return node->type == XML_ELEMENT_NODE &&
			strcmp(reinterpret_cast<const char *>(node->name), name) == 0;
}

std::string text(xmlNode *node) {
	xmlChar *content = xmlNodeGetContent(node);
	if (content == nullptr)
		return {};
	std::string s = reinterpret_cast<const char *>(content);
	xmlFree(content);
	std::size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return {};
	std::size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

bool parse_yes(const std::string &s, bool fallback) {
	if (s == "yes" || s == "true" || s == "on" || s == "1")
		return true;
	if (s == "no" || s == "false" || s == "off" || s == "0")
		return false;
	return fallback;
}

}  // namespace

std::optional<Action> parse_action_xml(xmlNode *action_node) {
	const char *name = attr(action_node, "name");
	const ActionSpec *spec = action_spec_from_name(name);
	if (spec == nullptr)
		return std::nullopt;

	std::string command;
	std::optional<Action> action;
	for (xmlNode *c = action_node->children; c; c = c->next) {
		/* <menu> is Openbox's spelling of ShowMenu's argument
		 * (<action name="ShowMenu"><menu>root-menu</menu></action>). Take
		 * the raw content so CDATA commands keep their whitespace. */
		if (spec->takes_command && command.empty() &&
				(is(c, "command") || is(c, "execute") || is(c, "menu")) &&
				c->children && c->children->content)
			command = reinterpret_cast<const char *>(c->children->content);
	}
	action = make_action(name, command);
	if (!action)
		return std::nullopt;

	for (xmlNode *c = action_node->children; c; c = c->next) {
		if (is(c, "to") || is(c, "desktop"))  /* <desktop>: Openbox 3.4 */
			action->to = text(c);
		else if (is(c, "wrap"))
			action->wrap = parse_yes(text(c), action->wrap);
		else if (is(c, "follow"))
			action->follow = parse_yes(text(c), action->follow);
	}
	return action;
}

void collect_actions_xml(xmlNode *first, std::vector<Action> &out, bool recurse) {
	for (xmlNode *node = first; node; node = node->next) {
		if (is(node, "action")) {
			if (auto action = parse_action_xml(node))
				out.push_back(std::move(*action));
		} else if (recurse && node->type == XML_ELEMENT_NODE &&
				!is(node, "keybind") && node->children) {
			collect_actions_xml(node->children, out, recurse);
		}
	}
}

}  // namespace wb
