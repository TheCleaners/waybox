#include "wb_test.hpp"

#include "waybox/action.hpp"
#include "waybox/action_catalog.hpp"

using wb::BoundAction;

/* The catalog and the registry agree: every entry is an action Waybox
 * knows; every action Waybox knows (aliases too) is offered, but those the
 * editor leaves to rc.xml (If, ForEach, Stop). */
WB_TEST(catalog_covers_the_registry) {
	for (const wb::ActionInfo &a : wb::action_catalog()) {
		WB_CHECK(wb::action_spec_from_name(a.name) != nullptr);
		WB_CHECK(!a.label.empty() && !a.category.empty());
	}
	for (const wb::ActionSpec &s : wb::action_registry()) {
		std::string n(s.name);
		if (n == "If" || n == "ForEach" || n == "Stop")
			continue;
		if (wb::action_info(n) == nullptr)
			WB_CHECK(n == "(missing from the catalog)");
	}
	WB_CHECK(wb::action_info("MaximizeHorz") == wb::action_info("Maximize"));
	WB_CHECK(wb::action_info("DirectionalFocusNorth")->name == "DirectionalTargetWindow");
	WB_CHECK(wb::action_info("Nope") == nullptr);
}

WB_TEST(descriptions) {
	using wb::describe_action;
	WB_CHECK(describe_action({"Execute", {{"command", "foot"}}}) == "Run foot");
	WB_CHECK(describe_action({"Execute", {{"execute", "foot"}}}) == "Run foot");
	WB_CHECK(describe_action({"GoToDesktop", {{"to", "2"}}}) == "Go to desktop 2");
	/* No desktop given: the current one, so nothing moves. */
	WB_CHECK(describe_action({"SendToDesktop", {}}) == "Keep the window here (no desktop given)");
	WB_CHECK(describe_action({"GoToDesktop", {}}) == "Stay on this desktop (no desktop given)");
	WB_CHECK(describe_action({"GoToDesktop", {{"to", "right"}}}) == "Go to the next desktop");
	WB_CHECK(describe_action({"SendToDesktop", {{"to", "left"}}}) ==
			"Send to the previous desktop");
	WB_CHECK(describe_action({"SnapToEdge", {{"direction", "up-left"}}}) ==
			"Tile: top left quarter");
	WB_CHECK(describe_action({"MaximizeHorz", {}}) == "Maximize (across)");
	WB_CHECK(describe_action({"ShowMenu", {{"menu", "client-menu"}}}) == "Show the window menu");
	WB_CHECK(describe_action({"Close", {}}) == "Close the window");
	WB_CHECK(describe_action({"If", {}, true}) == "If (edit in rc.xml)");
	WB_CHECK(wb::describe_actions({{"Raise", {}}, {"Move", {}}}) ==
			"Raise, then move with the mouse");
}
