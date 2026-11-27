#include "wb_test.hpp"

#include "waybox/menu_doc.hpp"

using wb::MenuDocument;
using wb::MenuEntry;
using K = MenuEntry::Kind;

namespace {

const char *kMenu = R"(<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu xmlns="http://openbox.org/3.4/menu">
<!-- my menu -->
<menu id="root-menu" label="Waybox">
  <menu id="applications" label="Applications" icon="applications-other" type="applications"/>
  <separator/>
  <item label="Terminal" icon="utilities-terminal">
    <action name="Execute"><execute>foot</execute></action>
  </item>
  <!-- tools -->
  <menu id="tools" label="Tools">
    <item label="Calculator"><action name="Execute"><command>gnome-calculator</command></action></item>
    <separator label="More"/>
  </menu>
  <menu id="places"/>
  <menu id="recent" label="Recent" execute="recent-files-menu"/>
  <item label="Exit" weird="1"><action name="Exit"/></item>
</menu>
<menu id="places" label="Places">
  <item label="Home"><action name="Execute"><command>xdg-open ~</command></action></item>
</menu>
</openbox_menu>
)";

}  // namespace

WB_TEST(menu_read) {
	auto d = MenuDocument::from_string(kMenu);
	WB_CHECK(d.has_value());
	if (!d)
		return;
	WB_CHECK((d->menu_ids() == std::vector<std::string>{"root-menu", "places"}));
	auto root = d->menu("root-menu");
	WB_CHECK(root.has_value() && root->kind == K::Submenu && root->label == "Waybox");
	if (!root || root->children.size() != 7)
		return;
	const auto &c = root->children;
	WB_CHECK(c[0].kind == K::Applications && c[0].icon == "applications-other");
	WB_CHECK(c[1].kind == K::Separator && c[1].label.empty());
	WB_CHECK(c[2].kind == K::Item && c[2].label == "Terminal" && c[2].actions.size() == 1 &&
			c[2].actions[0].name == "Execute");
	WB_CHECK(c[3].kind == K::Submenu && c[3].id == "tools" && c[3].children.size() == 2);
	WB_CHECK(c[3].children[1].kind == K::Separator && c[3].children[1].label == "More");
	WB_CHECK(c[4].kind == K::Reference && c[4].id == "places");
	WB_CHECK(c[5].kind == K::Pipe && c[5].execute == "recent-files-menu");
	WB_CHECK(c[6].label == "Exit" && (c[6].kept == std::vector<std::string>{"weird"}));
	/* A menu defined inline is found by id too. */
	auto tools = d->menu("tools");
	WB_CHECK(tools.has_value() && tools->children.size() == 2);
	WB_CHECK(!d->menu("nope").has_value());
	WB_CHECK(!MenuDocument::from_string("<openbox_menu><menu").has_value());
	/* Unchanged, written back as it was read (the blank line after the
	 * declaration too). */
	std::string spaced = std::string(kMenu).replace(std::string(kMenu).find("\n"), 1, "\n\n");
	auto s = MenuDocument::from_string(spaced);
	WB_CHECK(s.has_value() && s->to_string() == spaced);
}

WB_TEST(menu_edit_in_place) {
	auto d = MenuDocument::from_string(kMenu);
	WB_CHECK(d.has_value());
	if (!d)
		return;
	/* An item's label, icon and command. */
	MenuEntry term = d->menu("root-menu")->children[2];
	term.label = "Console";
	term.icon = "";
	term.actions[0].options = {{"command", "alacritty"}};
	WB_CHECK(d->set("root-menu", {2}, term));
	/* Inside a submenu. */
	MenuEntry calc = d->menu("root-menu")->children[3].children[0];
	calc.label = "Calc";
	WB_CHECK(d->set("root-menu", {3, 0}, calc));
	std::string out = d->to_string();
	WB_CHECK(out.find("<!-- my menu -->") != std::string::npos);
	WB_CHECK(out.find("<!-- tools -->") != std::string::npos);
	WB_CHECK(out.find("label=\"Console\"") != std::string::npos);
	WB_CHECK(out.find("icon=\"utilities-terminal\"") == std::string::npos);
	WB_CHECK(out.find("<command>alacritty</command>") != std::string::npos);
	WB_CHECK(out.find("label=\"Calc\"") != std::string::npos);
	WB_CHECK(out.find("weird=\"1\"") != std::string::npos);
	auto again = MenuDocument::from_string(out);
	WB_CHECK(again.has_value() && again->menu("root-menu")->children[2] == term);

	/* Another kind: the pipe menu becomes an item. */
	MenuEntry lock;
	lock.kind = K::Item;
	lock.label = "Lock";
	lock.actions = {{"Execute", {{"command", "swaylock"}}, false}};
	WB_CHECK(d->set("root-menu", {5}, lock));
	auto root = d->menu("root-menu");
	WB_CHECK(root->children.size() == 7 && root->children[5] == lock);
	WB_CHECK(d->to_string().find("recent-files-menu") == std::string::npos);
	WB_CHECK(!d->set("root-menu", {9}, lock));
	WB_CHECK(!d->set("root-menu", {}, lock));
}

WB_TEST(menu_insert_move_remove) {
	auto d = MenuDocument::from_string(kMenu);
	WB_CHECK(d.has_value());
	if (!d)
		return;
	/* A new submenu with its entries, on lines of their own. */
	MenuEntry games;
	games.kind = K::Submenu;
	games.id = "games";
	games.label = "Games";
	MenuEntry chess;
	chess.kind = K::Item;
	chess.label = "Chess";
	chess.actions = {{"Execute", {{"command", "gnome-chess"}}, false}};
	games.children = {chess};
	WB_CHECK(d->insert("root-menu", {}, 2, games));
	auto root = d->menu("root-menu");
	WB_CHECK(root->children.size() == 8 && root->children[2] == games);
	WB_CHECK(d->to_string().find("\n  <menu id=\"games\" label=\"Games\">\n    <item "
			"label=\"Chess\">") != std::string::npos);
	/* Into a submenu, at its end. */
	MenuEntry sep;
	sep.kind = K::Separator;
	WB_CHECK(d->insert("root-menu", {4}, 2, sep));
	WB_CHECK(d->menu("tools")->children.size() == 3);
	WB_CHECK(!d->insert("root-menu", {4}, 9, sep));
	WB_CHECK(!d->insert("root-menu", {3}, 0, sep));  /* an item holds no entries */

	/* Moved: Terminal (now 3) into Tools, first. */
	WB_CHECK(d->move("root-menu", {3}, {3}, 0));  /* after it's out, Tools is 3 */
	root = d->menu("root-menu");
	WB_CHECK(root->children[3].id == "tools" && root->children[3].children[0].label == "Terminal");
	/* Out again, to the end of the menu. */
	WB_CHECK(d->move("root-menu", {3, 0}, {}, root->children.size()));
	root = d->menu("root-menu");
	WB_CHECK(root->children.back().label == "Terminal");
	/* Not into itself; and a bad target leaves it where it was. */
	WB_CHECK(!d->move("root-menu", {3}, {3}, 0));
	WB_CHECK(!d->move("root-menu", {0}, {42}, 0));
	root = d->menu("root-menu");
	WB_CHECK(root->children[0].kind == K::Applications && root->children[3].id == "tools");

	/* Removed. */
	size_t n = root->children.size();
	WB_CHECK(d->remove("root-menu", {1}));
	WB_CHECK(d->menu("root-menu")->children.size() == n - 1);
	WB_CHECK(!d->remove("root-menu", {99}));
	WB_CHECK(d->to_string().find("<!-- my menu -->") != std::string::npos);
	/* Still a menu Waybox reads. */
	WB_CHECK(MenuDocument::from_string(d->to_string()).has_value());
}
