#include "wb_test.hpp"

#include "waybox/keychain.hpp"

using wb::ChainOutcome;
using wb::KeyBinding;

namespace {

/* A leaf binding with one (dummy) action. */
KeyBinding leaf(uint32_t sym, uint32_t mods) {
	KeyBinding b;
	b.sym = sym;
	b.modifiers = mods;
	b.actions.push_back(wb::Action{wb::ActionType::Close, {}});
	return b;
}

}  // namespace

WB_TEST(chain_fires_on_leaf_match) {
	std::vector<KeyBinding> roots{leaf(0x61 /*a*/, 8 /*Alt*/)};
	auto step = wb::chain_step(roots, 0x61, 8);
	WB_CHECK(step.outcome == ChainOutcome::Fire);
	WB_CHECK(step.node == &roots[0]);
}

WB_TEST(chain_no_match_on_wrong_chord) {
	std::vector<KeyBinding> roots{leaf(0x61, 8)};
	WB_CHECK(wb::chain_step(roots, 0x62, 8).outcome == ChainOutcome::NoMatch);   /* wrong sym */
	WB_CHECK(wb::chain_step(roots, 0x61, 0).outcome == ChainOutcome::NoMatch);   /* wrong mods */
	WB_CHECK(wb::chain_step(roots, 0x61, 4).outcome == ChainOutcome::NoMatch);
}

WB_TEST(chain_descends_into_prefix) {
	/* root "W-a" (sym=a, mods=Logo=64) with a child "b" (sym=b, no mods) */
	KeyBinding root;
	root.sym = 0x61;       /* a */
	root.modifiers = 64;   /* Logo */
	root.children.push_back(leaf(0x62 /*b*/, 0));
	std::vector<KeyBinding> roots{std::move(root)};

	auto step = wb::chain_step(roots, 0x61, 64);
	WB_CHECK(step.outcome == ChainOutcome::Descend);
	WB_CHECK(step.node == &roots[0]);

	/* stepping into the prefix's children fires on the second key */
	auto inner = wb::chain_step(roots[0].children, 0x62, 0);
	WB_CHECK(inner.outcome == ChainOutcome::Fire);
	/* a non-matching second key yields NoMatch (caller resets the chain) */
	WB_CHECK(wb::chain_step(roots[0].children, 0x63, 0).outcome == ChainOutcome::NoMatch);
}

WB_TEST(chain_prefix_takes_precedence_over_actions) {
	/* a node that has both actions and children is treated as a chain prefix */
	KeyBinding root = leaf(0x61, 0);
	root.children.push_back(leaf(0x62, 0));
	std::vector<KeyBinding> roots{std::move(root)};
	WB_CHECK(wb::chain_step(roots, 0x61, 0).outcome == ChainOutcome::Descend);
}

WB_TEST(chain_empty_node_is_no_match) {
	KeyBinding root;  /* no actions, no children */
	root.sym = 0x61;
	std::vector<KeyBinding> roots{std::move(root)};
	WB_CHECK(wb::chain_step(roots, 0x61, 0).outcome == ChainOutcome::NoMatch);
}

WB_TEST(chain_first_match_wins) {
	std::vector<KeyBinding> roots{leaf(0x61, 0), leaf(0x61, 0)};
	WB_CHECK(wb::chain_step(roots, 0x61, 0).node == &roots[0]);
}
