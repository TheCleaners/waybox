#include "wb_test.hpp"

#include "waybox/action.hpp"

using namespace wb;

WB_TEST(registry_lookup_by_name) {
	const ActionSpec *exec = action_spec_from_name("Execute");
	WB_CHECK(exec != nullptr);
	WB_CHECK(exec->type == ActionType::Execute);
	WB_CHECK(exec->takes_command);

	const ActionSpec *close = action_spec_from_name("Close");
	WB_CHECK(close != nullptr);
	WB_CHECK(close->type == ActionType::Close);
	WB_CHECK(!close->takes_command);
}

WB_TEST(registry_rejects_unknown_and_is_case_sensitive) {
	WB_CHECK(action_spec_from_name("DoesNotExist") == nullptr);
	WB_CHECK(action_spec_from_name("") == nullptr);
	/* Openbox action names are case-sensitive. */
	WB_CHECK(action_spec_from_name("execute") == nullptr);
	WB_CHECK(action_spec_from_name("EXECUTE") == nullptr);
}

WB_TEST(make_action_captures_command_only_when_taken) {
	auto exec = make_action("Execute", "foot --hold");
	WB_CHECK(exec.has_value());
	WB_CHECK(exec->type == ActionType::Execute);
	WB_CHECK_EQ(exec->command, std::string("foot --hold"));

	/* A non-command action ignores any command text. */
	auto close = make_action("Close", "ignored");
	WB_CHECK(close.has_value());
	WB_CHECK(close->type == ActionType::Close);
	WB_CHECK(close->command.empty());
}

WB_TEST(make_action_rejects_unknown_names) {
	WB_CHECK(!make_action("Bogus", "").has_value());
	WB_CHECK(!make_action("", "x").has_value());
}

WB_TEST(every_registered_action_round_trips) {
	for (const ActionSpec &spec : action_registry()) {
		const ActionSpec *got = action_spec_from_name(spec.name);
		WB_CHECK(got != nullptr);
		if (got != nullptr) {
			WB_CHECK(got->type == spec.type);
			WB_CHECK(got->takes_command == spec.takes_command);
		}
		auto action = make_action(spec.name, "cmd");
		WB_CHECK(action.has_value());
		if (action.has_value())
			WB_CHECK(action->type == spec.type);
	}
}
