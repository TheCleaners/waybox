#include "wb_test.hpp"

#include "waybox/menu.hpp"

using wb::Menu;
using wb::MenuFile;
using wb::MenuItem;

static const char *kSample = R"(<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu xmlns="http://openbox.org/3.4/menu">
  <menu id="root-menu" label="Waybox">
    <item label="Terminal">
      <action name="Execute"><command>foot</command></action>
    </item>
    <separator/>
    <separator label="Apps"/>
    <menu id="apps" label="Applications">
      <item label="Browser">
        <action name="Execute"><command>firefox</command></action>
      </item>
    </menu>
    <item label="Reconfigure">
      <action name="Reconfigure"/>
    </item>
  </menu>
</openbox_menu>
)";

WB_TEST(parse_builds_root_and_submenu) {
	MenuFile f = wb::parse_menu_xml(kSample);
	const Menu *root = f.find("root-menu");
	WB_CHECK(root != nullptr);
	WB_CHECK(root->label == "Waybox");
	/* item, separator, separator, submenu, item */
	WB_CHECK(root->items.size() == 5);

	/* the submenu "apps" is registered as its own menu */
	const Menu *apps = f.find("apps");
	WB_CHECK(apps != nullptr);
	WB_CHECK(apps->items.size() == 1);
	WB_CHECK(apps->items[0].label == "Browser");
}

WB_TEST(parse_item_kinds_and_actions) {
	MenuFile f = wb::parse_menu_xml(kSample);
	const Menu *root = f.find("root-menu");
	WB_CHECK(root != nullptr);

	WB_CHECK(root->items[0].kind == MenuItem::Kind::Entry);
	WB_CHECK(root->items[0].label == "Terminal");
	WB_CHECK(root->items[0].actions.size() == 1);
	WB_CHECK(root->items[0].actions[0].type == wb::ActionType::Execute);
	WB_CHECK(root->items[0].actions[0].command == "foot");

	WB_CHECK(root->items[1].kind == MenuItem::Kind::Separator);
	WB_CHECK(root->items[2].kind == MenuItem::Kind::Separator);
	WB_CHECK(root->items[2].label == "Apps");  /* labelled separator */

	WB_CHECK(root->items[3].kind == MenuItem::Kind::Submenu);
	WB_CHECK(root->items[3].submenu_id == "apps");

	WB_CHECK(root->items[4].kind == MenuItem::Kind::Entry);
	WB_CHECK(root->items[4].actions.size() == 1);
	WB_CHECK(root->items[4].actions[0].type == wb::ActionType::Reconfigure);
}

WB_TEST(parse_empty_and_garbage_is_safe) {
	WB_CHECK(wb::parse_menu_xml("").menus.empty());
	WB_CHECK(wb::parse_menu_xml("not xml at all").find("root-menu") == nullptr);
}
