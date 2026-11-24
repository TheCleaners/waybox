#include "wb_test.hpp"

#include "waybox/menu.hpp"

using wb::Menu;
using wb::MenuFile;
using wb::MenuItem;

static const char *kSample = R"(<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu xmlns="http://openbox.org/3.4/menu">
  <menu id="root-menu" label="Waybox">
    <item label="Terminal" icon="utilities-terminal">
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
	WB_CHECK(root->items[0].icon == "utilities-terminal");

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

WB_TEST(parse_records_pipe_menu_command) {
	MenuFile f = wb::parse_menu_xml(R"(<openbox_menu>
  <menu id="root-menu" label="Root">
    <menu id="apps" label="Applications" execute="openbox-menu"/>
    <menu id="bare" execute="gen --bare"/>
  </menu>
</openbox_menu>)");
	const wb::Menu *apps = f.find("apps");
	WB_CHECK(apps != nullptr);
	WB_CHECK(apps->execute == "openbox-menu");
	WB_CHECK(apps->items.empty());
	/* A pipe menu without a label is still a definition, not a reference. */
	const wb::Menu *bare = f.find("bare");
	WB_CHECK(bare != nullptr && bare->execute == "gen --bare");
	const wb::Menu *root = f.find("root-menu");
	WB_CHECK(root != nullptr && root->items.size() == 2);
	WB_CHECK(root->items[0].kind == wb::MenuItem::Kind::Submenu);
	WB_CHECK(root->items[0].submenu_id == "apps");
}

WB_TEST(parse_pipe_menu_output) {
	MenuFile f = wb::parse_pipe_menu(R"(<openbox_pipe_menu>
  <item label="Terminal"><action name="Execute"><execute>foot</execute></action></item>
  <separator/>
  <menu id="games" label="Games">
    <item label="Chess"><action name="Execute"><execute>gnome-chess</execute></action></item>
  </menu>
  <menu id="more" label="More" execute="gen more"/>
</openbox_pipe_menu>)", "apps", "Applications");
	const wb::Menu *apps = f.find("apps");
	WB_CHECK(apps != nullptr);
	WB_CHECK(apps->label == "Applications");
	WB_CHECK(apps->items.size() == 4);
	WB_CHECK(apps->items[0].label == "Terminal");
	WB_CHECK(apps->items[0].actions.size() == 1);
	WB_CHECK(apps->items[1].kind == wb::MenuItem::Kind::Separator);
	WB_CHECK(apps->items[2].submenu_id == "games");
	const wb::Menu *games = f.find("games");
	WB_CHECK(games != nullptr && games->items.size() == 1);
	/* Pipe menus can nest further pipe menus. */
	const wb::Menu *more = f.find("more");
	WB_CHECK(more != nullptr && more->execute == "gen more");
}

WB_TEST(parse_pipe_menu_rejects_garbage) {
	WB_CHECK(wb::parse_pipe_menu("not xml at all", "x", "X").menus.empty());
	WB_CHECK(wb::parse_pipe_menu("", "x", "X").menus.empty());
	/* Truncated output (e.g. a script killed mid-write) is not accepted. */
	WB_CHECK(wb::parse_pipe_menu("<openbox_pipe_menu><item label=\"a\">", "x", "X")
			.menus.empty());
}

WB_TEST(parse_pipe_menu_cdata_command) {
	/* obamenu (and others) wrap commands in CDATA. */
	MenuFile f = wb::parse_pipe_menu(R"(<?xml version="1.0" encoding="UTF-8" ?>
<openbox_pipe_menu xmlns="http://openbox.org/">
<menu id="openbox-Office" label="Office">
<item label="Viewer"><action name="Execute"><command><![CDATA[papers --flag]]></command></action></item>
</menu>
</openbox_pipe_menu>)", "apps", "Applications");
	const wb::Menu *office = f.find("openbox-Office");
	WB_CHECK(office != nullptr && office->items.size() == 1);
	WB_CHECK(office->items[0].actions.size() == 1);
	WB_CHECK(office->items[0].actions[0].command == "papers --flag");
}

WB_TEST(parse_desktop_action_options) {
	MenuFile f = wb::parse_menu_xml(R"(<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu xmlns="http://openbox.org/3.4/menu">
  <menu id="root-menu" label="Root">
    <item label="Two"><action name="GoToDesktop"><to>2</to></action></item>
    <item label="Right"><action name="GoToDesktop"><to> right </to><wrap>no</wrap></action></item>
    <item label="Send"><action name="SendToDesktop"><to>3</to><follow>no</follow></action></item>
    <item label="Old"><action name="SendToDesktop"><desktop>last</desktop></action></item>
    <item label="Sticky"><action name="ToggleOmnipresent"/></item>
  </menu>
</openbox_menu>
)");
	const Menu *root = f.find("root-menu");
	WB_CHECK(root != nullptr && root->items.size() == 5);
	if (root == nullptr || root->items.size() != 5)
		return;
	const wb::Action &two = root->items[0].actions.at(0);
	WB_CHECK(two.type == wb::ActionType::GoToDesktop);
	WB_CHECK(two.to == "2" && two.wrap && two.follow);
	const wb::Action &right = root->items[1].actions.at(0);
	WB_CHECK(right.to == "right" && !right.wrap);
	const wb::Action &send = root->items[2].actions.at(0);
	WB_CHECK(send.type == wb::ActionType::SendToDesktop);
	WB_CHECK(send.to == "3" && !send.follow);
	WB_CHECK(root->items[3].actions.at(0).to == "last");
	WB_CHECK(root->items[4].actions.at(0).type ==
			wb::ActionType::ToggleOmnipresent);
}

WB_TEST(parse_command_text_is_joined_and_trimmed) {
	/* Indenting around a CDATA section, and a comment splitting the text,
	 * don't change the command (Openbox joins and trims it). */
	MenuFile f = wb::parse_menu_xml(R"(<openbox_menu><menu id="root-menu">
  <item label="A"><action name="Execute"><command>
    <![CDATA[foo && bar]]>
  </command></action></item>
  <item label="B"><action name="Execute"><execute>foo<!-- x --> bar</execute></action></item>
</menu></openbox_menu>)");
	const Menu *root = f.find("root-menu");
	WB_CHECK(root != nullptr && root->items.size() == 2);
	WB_CHECK(root->items[0].actions[0].command == "foo && bar");
	WB_CHECK(root->items[1].actions[0].command == "foo bar");
}
