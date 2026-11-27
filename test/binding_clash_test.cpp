#include "wb_test.hpp"

#include "waybox/binding_clash.hpp"

using Chains = std::vector<std::vector<std::string>>;

WB_TEST(keys_compared_as_pressed) {
	WB_CHECK(wb::canonical_key("W-d") == "W-d");
	WB_CHECK(wb::canonical_key("W-D") == "W-d");
	WB_CHECK(wb::canonical_key("S-W-d") == "W-S-d");
	WB_CHECK(wb::canonical_key("A-C-Delete") == "C-A-Delete");
	WB_CHECK(wb::canonical_key("F2") == "F2");
}

WB_TEST(same_keys_the_later_never_runs) {
	auto c = wb::key_clashes(Chains{{"W-d"}, {"W-e"}, {"S-W-D"}, {"W-S-d"}}, {});
	WB_CHECK(!c[0] && !c[1] && !c[2]);
	WB_CHECK(c[3] && c[3]->by == 2u && !c[3]->by_chain);
	WB_CHECK((c[3]->keys == std::vector<std::string>{"W-S-d"}));
}

WB_TEST(a_chain_and_its_start) {
	/* The single key first: the chain never starts. */
	auto c = wb::key_clashes(Chains{{"C-a"}, {"C-a", "x"}}, {});
	WB_CHECK(!c[0] && c[1] && c[1]->by == 0u && !c[1]->by_chain);
	WB_CHECK((c[1]->keys == std::vector<std::string>{"C-a"}));
	/* The chain first: the single key starts it instead of running. */
	c = wb::key_clashes(Chains{{"C-a", "x"}, {"C-a"}}, {});
	WB_CHECK(!c[0] && c[1] && c[1]->by == 0u && c[1]->by_chain);
	/* Chains sharing only their start: both run. */
	c = wb::key_clashes(Chains{{"C-a", "x"}, {"C-a", "y"}, {"C-b"}}, {});
	WB_CHECK(!c[0] && !c[1] && !c[2]);
	/* Not given keys yet: no clash either way. */
	c = wb::key_clashes(Chains{{}, {"W-d"}, {}}, {});
	WB_CHECK(!c[0] && !c[1] && !c[2]);
}

WB_TEST(scripts_come_first) {
	std::string lua =
		"-- wb.bind(\"W-q\", nothing)  (a comment)\n"
		"wb.bind(\"W-x\", function() end)\n"
		"local k = 'W-z'\n"
		"wb.bind( 'C-A-t' , function() wb.action('Close') end)\n"
		"wb.bind(k, f)  -- built at run time: unseen\n";
	auto keys = wb::script_keys(lua, "init.lua");
	WB_CHECK(keys.size() == 2);
	WB_CHECK(keys[0].key == "W-x" && keys[0].line == 2 && keys[0].file == "init.lua");
	WB_CHECK(keys[1].key == "C-A-t" && keys[1].line == 4);
	auto c = wb::key_clashes(Chains{{"W-x"}, {"A-C-t", "y"}, {"W-y"}}, keys);
	WB_CHECK(c[0] && c[0]->script && c[0]->script->line == 2 && !c[0]->by);
	WB_CHECK(c[1] && c[1]->script && c[1]->script->key == "C-A-t");
	WB_CHECK(!c[2]);
}

WB_TEST(mouse_same_place) {
	auto c = wb::mouse_clashes({{"Root", "Right", "Press"}, {"Root", "Right", "Click"},
			{"Titlebar", "A-Left", "Drag"}, {"Root", "Right", "Press"},
			{"Titlebar", "a-left", "Drag"}});
	WB_CHECK(!c[0] && !c[1] && !c[2]);
	WB_CHECK(c[3] && *c[3] == 0u);
	/* Modifiers as pressed; the button's name as written. */
	WB_CHECK(!c[4]);
}
