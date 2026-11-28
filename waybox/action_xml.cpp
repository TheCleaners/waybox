/*
 * <action> element parsing shared by rc.xml and menu.xml (libxml).
 */
#include "waybox/action_xml.hpp"

#include "waybox/conditions.hpp"

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

/* A yes/no condition: <shaded>yes</shaded>. */
void condition(xmlNode *c, const char *name, std::optional<bool> &out) {
	if (is(c, name)) {
		std::string t = text(c);
		if (t == "yes" || t == "true" || t == "on" || t == "1")
			out = true;
		else if (t == "no" || t == "false" || t == "off" || t == "0")
			out = false;
	}
}

int positive(const std::string &s) {
	int n = 0;
	for (char ch : s) {
		if (ch < '0' || ch > '9' || n > 100000)
			return 0;
		n = n * 10 + (ch - '0');
	}
	return n;
}

/* One query from the condition elements among `first` and its siblings. */
Query parse_query(xmlNode *first, bool focus_target) {
	Query q;
	q.focus_target = focus_target;
	for (xmlNode *c = first; c; c = c->next) {
		if (c->type != XML_ELEMENT_NODE)
			continue;
		condition(c, "shaded", q.shaded);
		condition(c, "maximized", q.maximized);
		condition(c, "maximizedhorizontal", q.maximized_horizontal);
		condition(c, "maximizedvertical", q.maximized_vertical);
		condition(c, "iconified", q.iconified);
		condition(c, "fullscreen", q.fullscreen);
		condition(c, "focused", q.focused);
		condition(c, "focusable", q.focusable);
		condition(c, "urgent", q.urgent);
		condition(c, "undecorated", q.undecorated);
		condition(c, "omnipresent", q.omnipresent);
		if (is(c, "desktop")) {
			std::string t = text(c);
			if (t == "current")
				q.desktop = Query::Desktop::Current;
			else if (t == "other")
				q.desktop = Query::Desktop::Other;
			else if (t == "last")
				q.desktop = Query::Desktop::Last;
			else if (int n = positive(t); n > 0) {
				q.desktop = Query::Desktop::Number;
				q.desktop_number = n;
			}
		} else if (is(c, "activedesktop")) {
			q.active_desktop = positive(text(c));
		} else if (is(c, "monitor")) {
			q.monitor = positive(text(c));
		} else if (is(c, "title")) {
			q.title = make_text_match(attr(c, "type"), text(c));
		} else if (is(c, "class")) {
			q.cls = make_text_match(attr(c, "type"), text(c));
		} else if (is(c, "name")) {
			q.name = make_text_match(attr(c, "type"), text(c));
		} else if (is(c, "role")) {
			q.role = make_text_match(attr(c, "type"), text(c));
		} else if (is(c, "type")) {
			q.type = make_text_match(attr(c, "type"), text(c));
		}
	}
	return q;
}

/* The <action>s directly in a branch (<then>, <else>, <none>). */
std::vector<Action> branch(xmlNode *node, int depth);

std::optional<Action> parse_action_depth(xmlNode *action_node, int depth);

std::vector<Action> branch(xmlNode *node, int depth) {
	std::vector<Action> out;
	for (xmlNode *c = node->children; c; c = c->next)
		if (is(c, "action"))
			if (auto a = parse_action_depth(c, depth + 1))
				out.push_back(std::move(*a));
	return out;
}

/* Deep enough for any real configuration; past it, If and ForEach are
 * dropped (a config file must not nest without bound). */
constexpr int kMaxNesting = 16;

}  // namespace

std::optional<Action> parse_action_xml(xmlNode *action_node) {
	return parse_action_depth(action_node, 0);
}

namespace {

std::optional<Action> parse_action_depth(xmlNode *action_node, int depth) {
	const char *name = attr(action_node, "name");
	const ActionSpec *spec = action_spec_from_name(name);
	if (spec == nullptr)
		return std::nullopt;

	std::string command;
	std::optional<Action> action;
	for (xmlNode *c = action_node->children; c; c = c->next) {
		/* <menu> is Openbox's spelling of ShowMenu's argument
		 * (<action name="ShowMenu"><menu>root-menu</menu></action>). All
		 * the text and CDATA, trimmed (as Openbox): indenting around a
		 * CDATA section mustn't become the command. */
		if (spec->takes_command && command.empty() &&
				(is(c, "command") || is(c, "execute") || is(c, "menu")))
			command = text(c);
	}
	action = make_action(name, command);
	if (!action)
		return std::nullopt;

	for (xmlNode *c = action_node->children; c; c = c->next) {
		if (is(c, "to") || is(c, "desktop"))  /* <desktop>: Openbox 3.4 */
			action->to = text(c);
		else if (is(c, "wrap"))
			action->wrap = parse_yes(text(c), action->wrap);
		else if (is(c, "direction") || is(c, "where"))  /* <where>: Add/RemoveDesktop */
			action->direction = text(c);
		else if (is(c, "follow"))
			action->follow = parse_yes(text(c), action->follow);
		else if (is(c, "position")) {
			/* ShowMenu's <position><x/><y/><monitor/></position> (Openbox):
			 * "default" is as if not given. */
			for (xmlNode *p = c->children; p; p = p->next)
				for (const char *k : {"x", "y", "monitor"})
					if (is(p, k) && text(p) != "default")
						action->args[std::string("position.") + k] = text(p);
		} else if (c->type == XML_ELEMENT_NODE)
			for (const char *k : {"x", "y", "width", "height", "monitor", "left", "right",
					"top", "bottom", "prompt"})  /* <prompt>: Exit's */
				if (is(c, k))
					action->args[k] = text(c);
	}
	if (action->type == ActionType::If || action->type == ActionType::ForEach) {
		if (depth >= kMaxNesting)
			return std::nullopt;
		auto cond = std::make_shared<Conditional>();
		bool queries = false;
		for (xmlNode *c = action_node->children; c; c = c->next) {
			if (is(c, "query")) {
				queries = true;
				cond->queries.push_back(parse_query(c->children,
						std::string(attr(c, "target")).find("focus") != std::string::npos));
			} else if (is(c, "then")) {
				cond->then_actions = branch(c, depth);
			} else if (is(c, "else")) {
				cond->else_actions = branch(c, depth);
			} else if (is(c, "none")) {
				cond->none_actions = branch(c, depth);
			}
		}
		/* Without <query>, the conditions sit in the action itself. */
		if (!queries)
			cond->queries.push_back(parse_query(action_node->children, false));
		action->conditional = std::move(cond);
	}
	return action;
}

}  // namespace

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
