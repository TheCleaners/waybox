/*
 * Pure key-chain stepping. Standard-library only, so it links into the
 * standalone unit test.
 */
#include "waybox/keychain.hpp"

namespace wb {

ChainStep chain_step(const std::vector<KeyBinding> &children, uint32_t sym,
		uint32_t modifiers) {
	for (const KeyBinding &child : children) {
		if (child.sym != sym || child.modifiers != modifiers)
			continue;
		if (!child.children.empty())
			return {ChainOutcome::Descend, &child};
		if (!child.actions.empty())
			return {ChainOutcome::Fire, &child};
		return {ChainOutcome::NoMatch, nullptr};
	}
	return {ChainOutcome::NoMatch, nullptr};
}

}  // namespace wb
