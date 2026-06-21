/*
 * Openbox menu.xml parsing (libxml). Produces the pure wb::MenuFile model.
 */
#include "waybox/menu.hpp"

#include <cstring>
#include <string>

#include <libxml/parser.h>
#include <libxml/tree.h>

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

std::string action_command(xmlNode *action_node) {
	for (xmlNode *c = action_node->children; c; c = c->next) {
		if (c->type != XML_ELEMENT_NODE)
			continue;
		const char *tag = reinterpret_cast<const char *>(c->name);
		if ((strcmp(tag, "command") == 0 || strcmp(tag, "execute") == 0) &&
				c->children && c->children->content)
			return reinterpret_cast<const char *>(c->children->content);
	}
	return {};
}

void collect_actions(xmlNode *item, std::vector<Action> &out) {
	for (xmlNode *c = item->children; c; c = c->next) {
		if (c->type != XML_ELEMENT_NODE ||
				strcmp(reinterpret_cast<const char *>(c->name), "action") != 0)
			continue;
		const char *name = attr(c, "name");
		const ActionSpec *spec = action_spec_from_name(name);
		std::string command =
			(spec && spec->takes_command) ? action_command(c) : std::string{};
		if (auto action = make_action(name, command))
			out.push_back(std::move(*action));
	}
}

bool has_element_children(xmlNode *node) {
	for (xmlNode *c = node->children; c; c = c->next) {
		if (c->type == XML_ELEMENT_NODE)
			return true;
	}
	return false;
}

/* Parse a <menu> definition into a Menu, registering nested menu definitions
 * into `file` and adding submenu references to the parent's items. */
void parse_menu_node(xmlNode *menu_node, MenuFile &file) {
	Menu menu;
	menu.id = attr(menu_node, "id");
	menu.label = attr(menu_node, "label");

	for (xmlNode *c = menu_node->children; c; c = c->next) {
		if (c->type != XML_ELEMENT_NODE)
			continue;
		const char *tag = reinterpret_cast<const char *>(c->name);

		if (strcmp(tag, "item") == 0) {
			MenuItem item;
			item.kind = MenuItem::Kind::Entry;
			item.label = attr(c, "label");
			item.icon = attr(c, "icon");
			collect_actions(c, item.actions);
			menu.items.push_back(std::move(item));
		} else if (strcmp(tag, "separator") == 0) {
			MenuItem item;
			item.kind = MenuItem::Kind::Separator;
			item.label = attr(c, "label");
			menu.items.push_back(std::move(item));
		} else if (strcmp(tag, "menu") == 0) {
			/* A <menu> with a label or children defines a submenu; a bare
			 * <menu id="..."/> just references one. Either way it shows as a
			 * submenu item pointing at the id. */
			std::string sub_id = attr(c, "id");
			if (has_element_children(c) || attr(c, "label")[0] != '\0')
				parse_menu_node(c, file);
			MenuItem item;
			item.kind = MenuItem::Kind::Submenu;
			item.label = attr(c, "label");
			item.icon = attr(c, "icon");
			item.submenu_id = sub_id;
			menu.items.push_back(std::move(item));
		}
	}

	file.menus.push_back(std::move(menu));
}

}  // namespace

MenuFile parse_menu_xml(std::string_view contents) {
	MenuFile file;
	xmlDoc *doc = xmlReadMemory(contents.data(), static_cast<int>(contents.size()),
			"menu.xml", nullptr, XML_PARSE_RECOVER | XML_PARSE_NONET);
	if (doc == nullptr)
		return file;

	if (xmlNode *root = xmlDocGetRootElement(doc)) {
		for (xmlNode *c = root->children; c; c = c->next) {
			if (c->type == XML_ELEMENT_NODE &&
					strcmp(reinterpret_cast<const char *>(c->name), "menu") == 0)
				parse_menu_node(c, file);
		}
	}
	xmlFreeDoc(doc);
	return file;
}

}  // namespace wb
