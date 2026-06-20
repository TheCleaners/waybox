#ifndef WB_KEYCHAIN_HPP
#define WB_KEYCHAIN_HPP

#include <cstdint>
#include <vector>

#include "waybox/action.hpp"

namespace wb {

/*
 * Key-binding tree (Openbox keytree). A binding has a chord (sym + modifiers)
 * and either runs actions (a leaf) or holds child bindings that continue a
 * chain (e.g. "W-a" then "b"). The runtime keeps a pointer to the current node
 * and steps it on each keypress; the stepping decision is pure and unit-tested
 * here, while the live dispatch lives in seat.cpp.
 */
struct KeyBinding {
	uint32_t sym = 0;
	uint32_t modifiers = 0;
	std::vector<Action> actions;
	std::vector<KeyBinding> children;
};

enum class ChainOutcome {
	NoMatch,  /* no child matches the chord */
	Descend,  /* matched a chain prefix (a node with children) */
	Fire,     /* matched a leaf with actions to run */
};

struct ChainStep {
	ChainOutcome outcome = ChainOutcome::NoMatch;
	const KeyBinding *node = nullptr;  /* the matched child for Descend/Fire */
};

/*
 * Find the child of `children` whose chord equals (sym, modifiers) and
 * classify it: Descend if it has children (a chain prefix takes precedence),
 * Fire if it is a leaf with actions, NoMatch otherwise.
 */
ChainStep chain_step(const std::vector<KeyBinding> &children, uint32_t sym,
		uint32_t modifiers);

}  // namespace wb

#endif /* WB_KEYCHAIN_HPP */
