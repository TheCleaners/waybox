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

/* ---- Safety: strict loading, writing only what changed, safe saving ---- */

#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

WB_TEST(strict_load_refuses_broken_xml) {
	/* Each of these "parses" in libxml2's recover mode, dropping or changing
	 * part of the file; saving that would lose the user's config. */
	const char *broken[] = {
		/* a forgotten closing quote */
		"<openbox_config><keyboard><keybind key=\"W-y><action name=\"Close\"/>"
		"</keybind></keyboard><mouse><context name=\"Root\"/></mouse></openbox_config>",
		/* an unescaped & in a command */
		"<openbox_config><keyboard><keybind key=\"W-x\"><action name=\"Execute\">"
		"<command>a && b</command></action></keybind></keyboard></openbox_config>",
		/* not XML */
		"not xml at all",
		/* truncated */
		"<openbox_config><keyboard>",
	};
	for (const char *xml : broken) {
		wb::LoadError err;
		WB_CHECK(!RcDocument::from_string(xml, &err).has_value());
		WB_CHECK(!err.message.empty());
	}
	wb::LoadError err;
	RcDocument::from_string("<a>\n<b>\n</a>", &err);
	WB_CHECK(err.line >= 2);
}

static const char *kUserRc =
		"<?xml version=\"1.0\"?>\n"
		"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
		"  <placement><policy>Centre</policy></placement>\n"
		"  <theme>\n"
		"    <name>Clearlooks</name>\n"
		"    <font place=\"OnScreenDisplay\"><name>3270 Nerd Font</name><size>11</size></font>\n"
		"  </theme>\n"
		"</openbox_config>\n";

WB_TEST(apply_writes_only_what_changed) {
	auto doc = RcDocument::from_string(kUserRc);
	WB_CHECK(doc.has_value());
	/* What an editor shows: the file's values, defaults for the rest. */
	WayboxSettings shown = doc->read();
	shown.placement_policy = "Smart";  /* "Centre" isn't in its list */
	shown.margin_top = 0;
	shown.titlebar_pad_y = 2;
	shown.menu_wrap = true;
	WayboxSettings edited = shown;
	edited.margin_left = 7;
	doc->apply(edited, shown);
	std::string out = doc->to_string();
	WB_CHECK(out.find("<left>7</left>") != std::string::npos);
	/* Untouched: neither the shown defaults nor the value it couldn't show. */
	WB_CHECK(out.find("<policy>Centre</policy>") != std::string::npos);
	WB_CHECK(out.find("<top>") == std::string::npos);
	WB_CHECK(out.find("paddingY") == std::string::npos);
	WB_CHECK(out.find("wrap") == std::string::npos);
	WB_CHECK(out.find("<name>3270 Nerd Font</name>") != std::string::npos);
}

WB_TEST(fonts_keep_numbers_in_family_and_their_place) {
	auto doc = RcDocument::from_string(kUserRc);
	WayboxSettings s = doc->read();
	/* The OSD font under another accepted place name is found... */
	WB_CHECK(s.font_osd == std::optional<std::string>("3270 Nerd Font 11"));
	/* ...and written back into the same element. */
	WayboxSettings before = s;
	s.font_osd = "3270 Nerd Font Bold 12";
	doc->apply(s, before);
	std::string out = doc->to_string();
	WB_CHECK(out.find("<name>3270 Nerd Font</name>") != std::string::npos);
	WB_CHECK(out.find("<size>12</size>") != std::string::npos);
	WB_CHECK(out.find("ActiveOnScreenDisplay") == std::string::npos);
	size_t first = out.find("<font ");
	WB_CHECK(first != std::string::npos && out.find("<font ", first + 1) == std::string::npos);
}

static std::string slurp(const std::filesystem::path &p) {
	std::ifstream in(p);
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

WB_TEST(save_keeps_a_backup_the_symlink_and_the_mode) {
	namespace fs = std::filesystem;
	char tmpl[] = "/tmp/wbconf-test-XXXXXX";
	WB_CHECK(mkdtemp(tmpl) != nullptr);
	fs::path dir = tmpl;
	fs::path real = dir / "dotfiles" / "rc.xml";
	fs::create_directories(real.parent_path());
	{
		std::ofstream(real) << kUserRc;
	}
	::chmod(real.c_str(), 0600);
	fs::path link = dir / "rc.xml";
	fs::create_symlink(real, link);

	auto doc = RcDocument::load_file(link.string());
	WB_CHECK(doc.has_value());
	WayboxSettings before = doc->read(), s = before;
	s.margin_top = 9;
	doc->apply(s, before);
	std::string err;
	WB_CHECK(doc->save_file(link.string(), &err));
	WB_CHECK(err.empty());

	WB_CHECK(fs::is_symlink(link));                       /* the link survives */
	WB_CHECK(slurp(real).find("<top>9</top>") != std::string::npos);
	WB_CHECK(slurp(fs::path(real.string() + ".bak")) == kUserRc);  /* old file kept */
	struct stat st {};
	WB_CHECK(::stat(real.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
	WB_CHECK(!fs::exists(real.string() + ".tmp"));
	fs::remove_all(dir);
}
