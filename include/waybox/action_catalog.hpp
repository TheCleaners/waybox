#ifndef WB_ACTION_CATALOG_HPP
#define WB_ACTION_CATALOG_HPP

/*
 * The actions as wbconf's binding editor offers them: each canonical rc.xml
 * name (not the older aliases) with a label, a category and its options as
 * typed fields; and a one-line description of a bound action ("Go to
 * desktop 2", "Run foot"). Pure (test/action_catalog_test.cpp).
 */
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wb {

enum class OptionKind { Text, Number, Choice, Bool, Command };

struct ActionOption {
	std::string name;    /* the element: "to", "direction", "command" */
	std::string label;
	OptionKind kind = OptionKind::Text;
	std::vector<std::pair<std::string, std::string>> choices;  /* value, label */
	std::string fallback;  /* what's meant when it's left out */
};

struct ActionInfo {
	std::string name;      /* rc.xml's */
	std::string label;     /* "Go to desktop" */
	std::string category;  /* "Desktops" */
	std::vector<ActionOption> options;
};

/* Every action the editor offers, by category, then as listed. */
const std::vector<ActionInfo> &action_catalog();
/* An action's entry by rc.xml name, aliases included (MaximizeFull is
 * Maximize, direction both); nullptr if unknown. */
const ActionInfo *action_info(std::string_view name);

/* One bound action: its name and options as rc.xml has them. */
struct BoundAction {
	std::string name;
	std::vector<std::pair<std::string, std::string>> options;
	/* If/ForEach and anything else the editor doesn't model: kept as the
	 * XML it was, not editable here. */
	bool advanced = false;
};

/* "Go to desktop 2", "Run foot", "Snap to the left half". */
std::string describe_action(const BoundAction &a);
/* Several, as a key does them in order: "Raise, then Move". */
std::string describe_actions(const std::vector<BoundAction> &a);

}  // namespace wb

#endif /* WB_ACTION_CATALOG_HPP */
