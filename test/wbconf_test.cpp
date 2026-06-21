#include "wb_test.hpp"

#include <string>

#include "waybox/wbconf.hpp"

using wb::RcDocument;
using wb::WayboxSettings;

static const char *kMinimalRc =
		"<?xml version=\"1.0\"?>\n"
		"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
		"  <keyboard>\n"
		"    <keybind key=\"A-Tab\"><action name=\"NextWindow\"/></keybind>\n"
		"  </keyboard>\n"
		"  <theme><name>Clearlooks</name></theme>\n"
		"</openbox_config>\n";

WB_TEST(reads_existing_values) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WB_CHECK(doc.has_value());
	WayboxSettings s = doc->read();
	WB_CHECK(s.theme_name.has_value() && *s.theme_name == "Clearlooks");
	/* Nothing else is present yet. */
	WB_CHECK(!s.titlebar_pad_y.has_value());
	WB_CHECK(!s.switcher_order.has_value());
	WB_CHECK(!s.margin_top.has_value());
}

WB_TEST(round_trips_all_settings) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WB_CHECK(doc.has_value());

	WayboxSettings s;
	s.theme_name = "Onyx-Citrus";
	s.titlebar_pad_y = 2;
	s.titlebar_button_size = 0;
	s.titlebar_resize_grab = 8;
	s.placement_policy = "Center";
	s.margin_top = 24;
	s.margin_bottom = 0;
	s.margin_left = 4;
	s.margin_right = 4;
	s.menu_source = "builtin";
	s.menu_submenu_open = "hover";
	s.menu_hover_delay = 120;
	s.menu_wrap = true;
	s.menu_icons = false;
	s.switcher_order = "mru";
	s.switcher_osd = true;
	s.switcher_wrap = false;
	doc->apply(s);

	/* Re-parse the serialised document and confirm every value survived. */
	std::string xml = doc->to_string();
	auto doc2 = RcDocument::from_string(xml);
	WB_CHECK(doc2.has_value());
	WayboxSettings r = doc2->read();
	WB_CHECK(r.theme_name && *r.theme_name == "Onyx-Citrus");
	WB_CHECK(r.titlebar_pad_y && *r.titlebar_pad_y == 2);
	WB_CHECK(r.titlebar_button_size && *r.titlebar_button_size == 0);
	WB_CHECK(r.titlebar_resize_grab && *r.titlebar_resize_grab == 8);
	WB_CHECK(r.placement_policy && *r.placement_policy == "Center");
	WB_CHECK(r.margin_top && *r.margin_top == 24);
	WB_CHECK(r.margin_left && *r.margin_left == 4);
	WB_CHECK(r.menu_source && *r.menu_source == "builtin");
	WB_CHECK(r.menu_submenu_open && *r.menu_submenu_open == "hover");
	WB_CHECK(r.menu_hover_delay && *r.menu_hover_delay == 120);
	WB_CHECK(r.menu_wrap && *r.menu_wrap == true);
	WB_CHECK(r.menu_icons && *r.menu_icons == false);
	WB_CHECK(r.switcher_order && *r.switcher_order == "mru");
	WB_CHECK(r.switcher_osd && *r.switcher_osd == true);
	WB_CHECK(r.switcher_wrap && *r.switcher_wrap == false);

	/* The unrelated keybinding must be preserved. */
	WB_CHECK(xml.find("NextWindow") != std::string::npos);
}

WB_TEST(updates_in_place_without_duplicating) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WB_CHECK(doc.has_value());
	WayboxSettings s = doc->read();
	s.theme_name = "Bear2";
	doc->apply(s);
	std::string xml = doc->to_string();
	/* The theme name was replaced, not duplicated. */
	WB_CHECK(xml.find("Bear2") != std::string::npos);
	WB_CHECK(xml.find("Clearlooks") == std::string::npos);
	auto cnt = [](const std::string &h, const std::string &n) {
		size_t c = 0, p = 0;
		while ((p = h.find(n, p)) != std::string::npos) { ++c; p += n.size(); }
		return c;
	};
	WB_CHECK(cnt(xml, "<name>") == 1);
}

WB_TEST(clearing_removes_nodes_and_attrs) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WB_CHECK(doc.has_value());

	WayboxSettings set;
	set.theme_name = "X";
	set.menu_icons = true;
	doc->apply(set);
	WB_CHECK(doc->to_string().find("icons=") != std::string::npos);

	/* Re-applying with the fields cleared removes them. */
	WayboxSettings cleared = doc->read();
	cleared.theme_name = std::nullopt;
	cleared.menu_icons = std::nullopt;
	doc->apply(cleared);
	std::string xml = doc->to_string();
	WB_CHECK(xml.find("icons=") == std::string::npos);
	WB_CHECK(xml.find("<name>") == std::string::npos);
}

WB_TEST(round_trips_fonts) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WB_CHECK(doc.has_value());
	WayboxSettings s = doc->read();
	s.font_active_window = "Inter Bold 12";
	s.font_menu_item = "Cantarell 11";
	doc->apply(s);

	std::string xml = doc->to_string();
	/* Structured font element written under <theme>. */
	WB_CHECK(xml.find("place=\"ActiveWindow\"") != std::string::npos);
	WB_CHECK(xml.find("<name>Inter</name>") != std::string::npos);
	WB_CHECK(xml.find("<size>12</size>") != std::string::npos);
	WB_CHECK(xml.find("<weight>bold</weight>") != std::string::npos);

	auto doc2 = RcDocument::from_string(xml);
	WB_CHECK(doc2.has_value());
	WayboxSettings r = doc2->read();
	WB_CHECK(r.font_active_window && *r.font_active_window == "Inter Bold 12");
	WB_CHECK(r.font_menu_item && *r.font_menu_item == "Cantarell 11");
	WB_CHECK(!r.font_osd.has_value());

	/* Clearing removes the font element. */
	r.font_active_window = std::nullopt;
	doc2->apply(r);
	WB_CHECK(doc2->to_string().find("place=\"ActiveWindow\"") == std::string::npos);
}
