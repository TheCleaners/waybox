#include "wb_test.hpp"

#include "waybox/client_menu.hpp"

using wb::ActionType;
using wb::ClientMenuState;
using wb::MenuItem;

static const MenuItem *find_label(const wb::Menu &m, const std::string &label) {
	for (const MenuItem &i : m.items)
		if (i.label == label)
			return &i;
	return nullptr;
}

WB_TEST(entries_follow_the_window_state) {
	ClientMenuState s;
	s.desktop_names = {"web", "Desktop 2", "Desktop 3"};
	wb::MenuFile f = wb::build_client_menu(s);
	const wb::Menu *m = f.find(wb::kClientMenuId);
	WB_CHECK(m != nullptr);
	if (m == nullptr)
		return;
	WB_CHECK(find_label(*m, "Maximize") && find_label(*m, "Roll up"));
	WB_CHECK(find_label(*m, "Decorate") && find_label(*m, "Close"));
	WB_CHECK(find_label(*m, "Close")->actions.at(0).type == ActionType::Close);

	s.maximized = s.shaded = s.decorated = true;
	f = wb::build_client_menu(s);
	m = f.find(wb::kClientMenuId);
	WB_CHECK(find_label(*m, "Restore") && !find_label(*m, "Maximize"));
	WB_CHECK(find_label(*m, "Roll down")->actions.at(0).type == ActionType::Unshade);
	WB_CHECK(find_label(*m, "Undecorate") != nullptr);

	/* A client that draws its own decorations: no Decorate entry. */
	s.can_decorate = false;
	f = wb::build_client_menu(s);
	m = f.find(wb::kClientMenuId);
	WB_CHECK(!find_label(*m, "Undecorate") && !find_label(*m, "Decorate"));
}

WB_TEST(send_to_desktop_submenu) {
	ClientMenuState s;
	s.desktop_names = {"web", "code"};
	s.desktop = 1;
	wb::MenuFile f = wb::build_client_menu(s);
	const wb::Menu *d = f.find(wb::kClientMenuDesktopsId);
	WB_CHECK(d != nullptr);
	if (d == nullptr)
		return;
	const MenuItem *web = find_label(*d, "web");
	const MenuItem *code = find_label(*d, "• code");  /* current: marked */
	WB_CHECK(web && code);
	if (web && code) {
		WB_CHECK(web->actions.at(0).type == ActionType::SendToDesktop);
		WB_CHECK(web->actions.at(0).to == "1" && !web->actions.at(0).follow);
		WB_CHECK(code->actions.at(0).to == "2");
	}
	WB_CHECK(find_label(*d, "All desktops") != nullptr);
	s.omnipresent = true;
	f = wb::build_client_menu(s);
	d = f.find(wb::kClientMenuDesktopsId);
	WB_CHECK(find_label(*d, "• All desktops") && find_label(*d, "code"));
}

WB_TEST(single_desktop_has_no_submenu) {
	ClientMenuState s;
	s.desktop_names = {"Desktop 1"};
	wb::MenuFile f = wb::build_client_menu(s);
	const wb::Menu *m = f.find(wb::kClientMenuId);
	WB_CHECK(m && !find_label(*m, "Send to desktop"));
}

/* Openbox's Layer submenu: the window's layer marked; each sends it there. */
WB_TEST(layer_submenu) {
	ClientMenuState s;
	s.desktop_names = {"Desktop 1"};
	s.layer = 1;
	wb::MenuFile f = wb::build_client_menu(s);
	const wb::Menu *m = f.find(wb::kClientMenuId);
	const wb::Menu *l = f.find(wb::kClientMenuLayerId);
	WB_CHECK(m != nullptr && l != nullptr);
	if (m == nullptr || l == nullptr)
		return;
	const MenuItem *sub = find_label(*m, "Layer");
	WB_CHECK(sub && sub->kind == MenuItem::Kind::Submenu &&
			sub->submenu_id == wb::kClientMenuLayerId);
	const MenuItem *top = find_label(*l, "\u2022 Always on top");
	WB_CHECK(top && top->actions.at(0).type == ActionType::SendToLayer &&
			top->actions.at(0).direction == "top");
	const MenuItem *bottom = find_label(*l, "Always on bottom");
	WB_CHECK(bottom && bottom->actions.at(0).direction == "bottom");
	WB_CHECK(find_label(*l, "Normal") != nullptr);
}
