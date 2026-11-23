/*
 * Pure key-chain stepping. Standard-library only, so it links into the
 * standalone unit test.
 */
#include "waybox/keychain.hpp"

namespace wb {

/* Fold a keysym to lower case for Latin-1 letters (the only cased keysyms
 * whose upper/lower forms differ by a fixed offset). Openbox resolves key
 * names to keycodes, so "W-S-M" and "W-S-m" name the same key; matching
 * case-insensitively gives the same result whether the keymap reports the
 * shifted (M) or unshifted (m) keysym for Shift+m. The modifier mask still
 * distinguishes W-m from W-S-M. */
uint32_t keysym_fold_case(uint32_t sym) {
	if (sym >= 0x41 && sym <= 0x5a)                   /* A-Z */
		return sym + 0x20;
	if (sym >= 0xc0 && sym <= 0xde && sym != 0xd7)    /* Agrave-Thorn, not multiply */
		return sym + 0x20;
	return sym;
}

ChainStep chain_step(const std::vector<KeyBinding> &children, uint32_t sym,
		uint32_t modifiers) {
	for (const KeyBinding &child : children) {
		if (keysym_fold_case(child.sym) != keysym_fold_case(sym) ||
				child.modifiers != modifiers)
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
