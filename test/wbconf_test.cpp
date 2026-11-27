#include "wb_test.hpp"

#include <algorithm>

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

WB_TEST(round_trips_font_styles) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WB_CHECK(doc.has_value());
	WayboxSettings s = doc->read();
	/* Weight, italic and width as the font control writes them. */
	s.font_active_window = "Inter Semi-Bold Italic Condensed 11";
	s.font_menu_item = "Cantarell Light 10.5";
	doc->apply(s);
	std::string xml = doc->to_string();
	WB_CHECK(xml.find("<weight>semibold</weight>") != std::string::npos);
	WB_CHECK(xml.find("<slant>italic</slant>") != std::string::npos);
	WB_CHECK(xml.find("<width>condensed</width>") != std::string::npos);
	WB_CHECK(xml.find("<weight>light</weight>") != std::string::npos);
	WB_CHECK(xml.find("<size>11</size>") != std::string::npos);  /* 10.5: whole points */
	auto doc2 = RcDocument::from_string(xml);
	WayboxSettings r = doc2->read();
	WB_CHECK(r.font_active_window == std::optional<std::string>(
			"Inter Semi-Bold Italic Condensed 11"));
	/* Back to upright, normal width: <slant> kept as normal (it was there),
	 * <width> gone (Waybox's own). */
	r.font_active_window = "Inter Bold 11";
	doc2->apply(r);
	std::string again = doc2->to_string();
	WB_CHECK(again.find("<weight>bold</weight>") != std::string::npos);
	WB_CHECK(again.find("<slant>normal</slant>") != std::string::npos);
	WB_CHECK(again.find("<width>") == std::string::npos);
	/* Openbox's own: italic and normal, read. */
	auto ob = RcDocument::from_string(
		"<?xml version=\"1.0\"?>\n<openbox_config xmlns=\"http://openbox.org/3.4/rc\"><theme>"
		"<font place=\"MenuItem\"><name>Sans</name><size>9</size><weight>normal</weight>"
		"<slant>italic</slant></font></theme></openbox_config>\n");
	WB_CHECK(ob && ob->read().font_menu_item == std::optional<std::string>("Sans Italic 9"));
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

/* ---- Desktops, edge tiling, monitors ---- */

static const char *kMoreRc =
		"<?xml version=\"1.0\"?>\n"
		"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
		"  <desktops><number>3</number>"
		"<names><name>Mail</name><name>Code</name></names></desktops>\n"
		"  <waybox>\n"
		"    <edgeTiling enabled=\"no\" range=\"12\"/>\n"
		"    <outputs>\n"
		"      <!-- the TV -->\n"
		"      <output name=\"HDMI-A-1\" mode=\"3840x2160@60\" scale=\"1.5\"/>\n"
		"      <output name=\"DP-4\" position=\"3840,0\"/>\n"
		"      <output make=\"Dell Inc.\" name=\"DP-9\" scale=\"2\"/>\n"
		"    </outputs>\n"
		"  </waybox>\n"
		"</openbox_config>\n";

WB_TEST(reads_desktops_edge_tiling_and_outputs) {
	auto doc = RcDocument::from_string(kMoreRc);
	WB_CHECK(doc.has_value());
	WayboxSettings s = doc->read();
	WB_CHECK(s.desktops_number == std::optional<int>(3));
	WB_CHECK(s.desktop_names && s.desktop_names->size() == 2 &&
			(*s.desktop_names)[1] == "Code");
	WB_CHECK(!s.desktop_popup_time.has_value());
	WB_CHECK(s.edge_tiling == std::optional<bool>(false));
	WB_CHECK(s.edge_tiling_range == std::optional<int>(12));
	/* Every rule, in order; a matcher isn't an attribute. */
	WB_CHECK(s.outputs.size() == 3);
	WB_CHECK(s.outputs[0].name == "HDMI-A-1" &&
			s.outputs[0].attrs.at("mode") == "3840x2160@60" &&
			s.outputs[0].attrs.at("scale") == "1.5");
	WB_CHECK(s.outputs[1].name == "DP-4" && s.outputs[1].attrs.size() == 1);
	WB_CHECK(s.outputs[2].name == "DP-9" && s.outputs[2].make == "Dell Inc." &&
			s.outputs[2].attrs.size() == 1 && s.outputs[2].attrs.count("make") == 0);
	WB_CHECK((s.outputs[2].from == wb::OutputMatch{"DP-9", "Dell Inc.", "", ""}));
}

WB_TEST(writes_only_the_output_setting_that_changed) {
	auto doc = RcDocument::from_string(kMoreRc);
	WayboxSettings before = doc->read(), s = before;
	s.outputs[0].attrs["scale"] = "2";           /* change one attribute */
	s.outputs[1].attrs.clear();                   /* DP-4 back to automatic */
	s.outputs.push_back({"DP-1", {{"enabled", "no"}}});  /* a new one */
	s.desktop_popup_time = 0;
	s.desktop_names = std::vector<std::string>{"Mail", "Code", "Games"};
	doc->apply(s, before);
	std::string out = doc->to_string();
	WB_CHECK(out.find("mode=\"3840x2160@60\" scale=\"2\"") != std::string::npos);
	WB_CHECK(out.find("DP-4") == std::string::npos);  /* no settings left */
	WB_CHECK(out.find("<output name=\"DP-1\" enabled=\"no\"/>") != std::string::npos);
	WB_CHECK(out.find("make=\"Dell Inc.\" name=\"DP-9\" scale=\"2\"") != std::string::npos);
	WB_CHECK(out.find("<!-- the TV -->") != std::string::npos);
	WB_CHECK(out.find("<popupTime>0</popupTime>") != std::string::npos);
	WB_CHECK(out.find("<name>Games</name>") != std::string::npos);
	WB_CHECK(out.find("edgeTiling enabled=\"no\" range=\"12\"") != std::string::npos);

	/* Read back: what was written is what is read. */
	auto again = RcDocument::from_string(out);
	WayboxSettings r = again->read();
	WB_CHECK(r.outputs.size() == 3 && r.outputs[0].attrs.at("scale") == "2" &&
			r.outputs[1].name == "DP-9" && r.outputs[2].name == "DP-1");
	WB_CHECK(r.desktop_names && r.desktop_names->size() == 3);
}

/* ---- Keyboard, mouse, HDR and advanced settings ---- */

static const char *kInputRc =
		"<?xml version=\"1.0\"?>\n"
		"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
		"  <keyboard>\n"
		"    <xkb><layout>us,de</layout><options>grp:alt_shift_toggle</options></xkb>\n"
		"    <keybind key=\"W-x\"><action name=\"Close\"/></keybind>\n"
		"  </keyboard>\n"
		"  <mouse>\n"
		"    <libinput><naturalScroll>enabled</naturalScroll><accelSpeed>0.3</accelSpeed></libinput>\n"
		"    <context name=\"Root\"/>\n"
		"  </mouse>\n"
		"  <waybox><hdr outputs=\"HDMI-A-1\"/><xwayland enabled=\"no\"/></waybox>\n"
		"</openbox_config>\n";

WB_TEST(reads_keyboard_mouse_hdr_advanced) {
	auto doc = RcDocument::from_string(kInputRc);
	WayboxSettings s = doc->read();
	WB_CHECK(s.xkb.size() == 2 && s.xkb.at("layout") == "us,de" &&
			s.xkb.at("options") == "grp:alt_shift_toggle");
	WB_CHECK(s.libinput.size() == 2 && s.libinput.at("naturalScroll") == "enabled" &&
			s.libinput.at("accelSpeed") == "0.3");
	WB_CHECK(s.hdr_outputs == std::optional<std::string>("HDMI-A-1"));
	WB_CHECK(s.xwayland == std::optional<bool>(false));
	WB_CHECK(!s.virtual_input.has_value());
}

WB_TEST(writes_only_changed_input_settings) {
	auto doc = RcDocument::from_string(kInputRc);
	WayboxSettings before = doc->read(), s = before;
	s.xkb.erase("options");                     /* back to the default */
	s.libinput["tap"] = "enabled";              /* a new one */
	s.libinput["accelSpeed"] = "-0.2";          /* a changed one */
	s.virtual_input = "never";
	doc->apply(s, before);
	std::string out = doc->to_string();
	WB_CHECK(out.find("grp:alt_shift_toggle") == std::string::npos);
	WB_CHECK(out.find("<layout>us,de</layout>") != std::string::npos);
	WB_CHECK(out.find("<accelSpeed>-0.2</accelSpeed>") != std::string::npos);
	WB_CHECK(out.find("<tap>enabled</tap>") != std::string::npos);
	WB_CHECK(out.find("<naturalScroll>enabled</naturalScroll>") != std::string::npos);
	WB_CHECK(out.find("virtualInput allow=\"never\"") != std::string::npos);
	/* Bindings next to them are untouched. */
	WB_CHECK(out.find("<keybind key=\"W-x\">") != std::string::npos);
	WB_CHECK(out.find("<context name=\"Root\"/>") != std::string::npos);

	/* Everything back to defaults: the <xkb> and <libinput> blocks go. */
	WayboxSettings b2 = s, s2 = s;
	s2.xkb.clear();
	s2.libinput.clear();
	doc->apply(s2, b2);
	out = doc->to_string();
	WB_CHECK(out.find("<xkb") == std::string::npos);
	WB_CHECK(out.find("<libinput") == std::string::npos);
	WB_CHECK(out.find("<keybind key=\"W-x\">") != std::string::npos);
}

WB_TEST(title_layout_round_trip) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WayboxSettings before = doc->read(), s = before;
	WB_CHECK(!before.title_layout.has_value());
	s.title_layout = "NLSDIMC";
	doc->apply(s, before);
	std::string out = doc->to_string();
	WB_CHECK(out.find("<titleLayout>NLSDIMC</titleLayout>") != std::string::npos);
	WB_CHECK(out.find("<name>Clearlooks</name>") != std::string::npos);
	WB_CHECK(RcDocument::from_string(out)->read().title_layout ==
			std::optional<std::string>("NLSDIMC"));
}

WB_TEST(switcher_icons_round_trip) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WayboxSettings before = doc->read(), s = before;
	s.switcher_icons = false;
	doc->apply(s, before);
	std::string out = doc->to_string();
	WB_CHECK(out.find("icons=\"no\"") != std::string::npos);
	WB_CHECK(RcDocument::from_string(out)->read().switcher_icons ==
			std::optional<bool>(false));
}

WB_TEST(output_left_out_is_removed) {
	/* What wbconf does for a monitor set back to Automatic: it isn't in
	 * the settings any more. */
	auto doc = RcDocument::from_string(kMoreRc);
	WayboxSettings before = doc->read(), s = before;
	s.outputs.erase(s.outputs.begin());  /* HDMI-A-1 */
	doc->apply(s, before);
	std::string out = doc->to_string();
	WB_CHECK(out.find("HDMI-A-1") == std::string::npos);
	WB_CHECK(out.find("<output name=\"DP-4\" position=\"3840,0\"/>") != std::string::npos);
	WB_CHECK(out.find("make=\"Dell Inc.\"") != std::string::npos);
}

WB_TEST(startup_programs_read_and_written_in_place) {
	auto doc = RcDocument::from_string(
			"<?xml version=\"1.0\"?>\n"
			"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
			"  <waybox>\n"
			"    <startup>\n"
			"      <!-- my panel -->\n"
			"      <program name=\"panel\" restart=\"no\">waybar</program>\n"
			"      <program>/usr/bin/mako</program>\n"
			"      <program enabled=\"no\">wlsunset -l 1 -L 2</program>\n"
			"    </startup>\n"
			"    <idle screenOff=\"600\"/>\n"
			"  </waybox>\n"
			"</openbox_config>\n");
	WayboxSettings before = doc->read(), s = before;
	WB_CHECK(s.startup.size() == 3);
	WB_CHECK(s.startup[0].name == "panel" && s.startup[0].attrs.at("restart") == "no");
	WB_CHECK(s.startup[1].name == "mako" && s.startup[1].command == "/usr/bin/mako");
	WB_CHECK(s.startup[2].attrs.at("enabled") == "no");
	WB_CHECK(s.idle_screen_off == std::optional<int>(600));

	/* Turn wlsunset on with a new location, drop mako, add swaybg. */
	s.startup[2].attrs.erase("enabled");
	s.startup[2].command = "wlsunset -l 48.8 -L 2.3";
	s.startup.erase(s.startup.begin() + 1);
	s.startup.push_back({"wall", "swaybg -c '#333' & echo <x>", {{"name", "wall"}}});
	s.idle_screen_off.reset();
	doc->apply(s, before);
	std::string out = doc->to_string();
	WB_CHECK(out.find("<!-- my panel -->") != std::string::npos);  /* untouched */
	WB_CHECK(out.find("<program name=\"panel\" restart=\"no\">waybar</program>") !=
			std::string::npos);
	WB_CHECK(out.find("mako") == std::string::npos);
	WB_CHECK(out.find("<program>wlsunset -l 48.8 -L 2.3</program>") != std::string::npos);
	WB_CHECK(out.find("swaybg -c '#333' &amp; echo &lt;x&gt;") != std::string::npos);
	WB_CHECK(out.find("screenOff") == std::string::npos);
	WayboxSettings again = RcDocument::from_string(out)->read();
	WB_CHECK(again.startup == s.startup);
}

WB_TEST(startup_emptied_is_removed) {
	auto doc = RcDocument::from_string(
			"<?xml version=\"1.0\"?>\n"
			"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
			"  <waybox><startup><program>waybar</program></startup></waybox>\n"
			"</openbox_config>\n");
	WayboxSettings before = doc->read(), s = before;
	s.startup.clear();
	doc->apply(s, before);
	WB_CHECK(doc->to_string().find("startup") == std::string::npos);
	/* And nothing written when untouched. */
	auto doc2 = RcDocument::from_string(kMinimalRc);
	WayboxSettings b2 = doc2->read();
	doc2->apply(b2, b2);
	WB_CHECK(doc2->to_string().find("waybox") == std::string::npos);
}

WB_TEST(execute_keybinds_read_and_set) {
	auto doc = RcDocument::from_string(
			"<?xml version=\"1.0\"?>\n"
			"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
			"  <keyboard>\n"
			"    <!-- mine -->\n"
			"    <keybind key=\"W-Return\"><action name=\"Execute\"><command>foot</command></action></keybind>\n"
			"    <keybind key=\"A-Tab\"><action name=\"NextWindow\"/></keybind>\n"
			"    <keybind key=\"W-e\"><action name=\"Execute\"><execute>thunar</execute></action>"
			"<action name=\"Focus\"/></keybind>\n"
			"  </keyboard>\n"
			"</openbox_config>\n");
	auto binds = doc->execute_keybinds();
	WB_CHECK(binds.size() == 1);  /* W-e has two actions; A-Tab isn't Execute */
	WB_CHECK(binds[0].first == "W-Return" && binds[0].second == "foot");

	doc->set_keybind("W-period", "bemoji -t -n");           /* new */
	doc->set_keybind("W-Return", "alacritty");               /* changed */
	doc->set_keybind("A-Tab", std::nullopt);                 /* not Execute: kept */
	doc->set_keybind("W-x", std::nullopt);                   /* absent: nothing */
	std::string out = doc->to_string();
	WB_CHECK(out.find("<!-- mine -->") != std::string::npos);
	WB_CHECK(out.find("NextWindow") != std::string::npos);
	WB_CHECK(out.find("thunar") != std::string::npos);
	auto again = RcDocument::from_string(out)->execute_keybinds();
	WB_CHECK(again.size() == 2);
	WB_CHECK(again[0] == std::make_pair(std::string("W-Return"), std::string("alacritty")));
	WB_CHECK(again[1] == std::make_pair(std::string("W-period"), std::string("bemoji -t -n")));
	doc->set_keybind("W-period", std::nullopt);
	WB_CHECK(doc->to_string().find("W-period") == std::string::npos);
}

WB_TEST(input_method_read_and_written) {
	auto doc = RcDocument::from_string(kMinimalRc);
	WayboxSettings before = doc->read(), s = before;
	WB_CHECK(!s.input_method);
	s.input_method = wb::InputMethodSetting{"fcitx5 --replace",
			{{"XMODIFIERS", "@im=fcitx"}, {"QT_IM_MODULE", "fcitx"}}};
	doc->apply(s, before);
	std::string out = doc->to_string();
	WB_CHECK(out.find("<inputMethod command=\"fcitx5 --replace\">") != std::string::npos);
	WB_CHECK(out.find("<env name=\"XMODIFIERS\" value=\"@im=fcitx\"/>") != std::string::npos);
	auto again = RcDocument::from_string(out);
	WayboxSettings read = again->read();
	WB_CHECK(read.input_method == s.input_method);
	/* Back to none: the element goes. */
	WayboxSettings none = read;
	none.input_method.reset();
	again->apply(none, read);
	WB_CHECK(again->to_string().find("inputMethod") == std::string::npos);
}

/* ---- Output rules matched by make, model and serial ---- */

#include <random>

#include "waybox/output_rules.hpp"

namespace {

const char *kRulesRc =
		"<?xml version=\"1.0\"?>\n"
		"<openbox_config xmlns=\"http://openbox.org/3.4/rc\">\n"
		"  <waybox>\n"
		"    <outputs>\n"
		"      <output make=\"Dell Inc.\" model=\"DELL U2723QE\" serial=\"5XYZ\" scale=\"2\"/>\n"
		"      <output name=\"DP-3\" scale=\"1\" position=\"0,0\"/>\n"
		"      <output make=\"Dell Inc.\" model=\"DELL U2723QE\" serial=\"5XYZ\" scale=\"3\"/>\n"
		"      <output enabled=\"no\"/>\n"
		"      <output make=\"BOE\" model=\"Twin\" adaptiveSync=\"yes\"/>\n"
		"      <output name=\"HDMI-A-1\" transform=\"flipped-90\" note=\"a &amp; &lt;b&gt; &quot;x&quot;\"/>\n"
		"    </outputs>\n"
		"  </waybox>\n"
		"</openbox_config>\n";

wb::OutputMatch dell_on_dp3() { return {"DP-3", "Dell Inc.", "DELL U2723QE", "5XYZ"}; }

}  // namespace

WB_TEST(output_rules_read_as_waybox_uses_them) {
	auto doc = RcDocument::from_string(kRulesRc);
	WB_CHECK(doc.has_value());
	WayboxSettings s = doc->read();
	/* The second Dell rule (same matcher) and the one matching nothing are
	 * left out: they never apply. */
	WB_CHECK(s.outputs.size() == 4);
	WB_CHECK(s.outputs[0].serial == "5XYZ" && s.outputs[0].name.empty() &&
			s.outputs[0].attrs.at("scale") == "2");
	WB_CHECK(s.outputs[3].attrs.at("note") == "a & <b> \"x\"");
	/* The Dell on DP-3: the serial rule, first, wins over the name rule. */
	WB_CHECK(wb::applying_output_rule(s.outputs, dell_on_dp3()) == 0);
	/* Another monitor on DP-3: the name rule. */
	WB_CHECK(wb::applying_output_rule(s.outputs, {"DP-3", "LG", "27GL", "9"}) == 1);
	/* Two BOE "Twin" panels: both get the make+model rule. */
	WB_CHECK(wb::applying_output_rule(s.outputs, {"eDP-1", "BOE", "Twin", "A"}) == 2);
	WB_CHECK(wb::applying_output_rule(s.outputs, {"DP-7", "BOE", "Twin", "B"}) == 2);
	WB_CHECK(wb::applying_output_rule(s.outputs, {"DP-8", "", "", ""}) == -1);
	/* A monitor with no make/model/serial: a rule asking for one never
	 * matches it. */
	WB_CHECK(!wb::output_rule_matches({"", "", "", "5XYZ"}, {"DP-3", "", "", ""}));
	WB_CHECK(!wb::output_rule_matches({}, {"", "", "", ""}));
}

WB_TEST(output_rule_matching_agrees_with_waybox) {
	/* The same choice as the compositor's, for any rules and monitor. */
	std::mt19937 rng(235);
	const char *names[] = {"", "DP-1", "DP-2"}, *makes[] = {"", "Dell", "LG"},
			*models[] = {"", "A", "B"}, *serials[] = {"", "1", "2"};
	auto pick = [&](const char *const *v) { return std::string(v[rng() % 3]); };
	for (int round = 0; round < 3000; ++round) {
		std::vector<wb::OutputSetting> mine;
		std::vector<wb::OutputRule> theirs;
		int n = static_cast<int>(rng() % 5);
		for (int i = 0; i < n; ++i) {
			wb::OutputSetting o;
			o.name = pick(names);
			o.make = pick(makes);
			o.model = pick(models);
			o.serial = pick(serials);
			mine.push_back(o);
			wb::OutputRule r;
			r.match = {o.name, o.make, o.model, o.serial};
			theirs.push_back(r);
		}
		wb::OutputMatch m{pick(names + 1 - 1), pick(makes), pick(models), pick(serials)};
		if (m.name.empty())
			m.name = "DP-1";  /* a monitor always has a connector */
		const wb::OutputRule *r = wb::match_output(theirs, {m.name, m.make, m.model, m.serial});
		int want = r == nullptr ? -1 : static_cast<int>(r - theirs.data());
		if (wb::applying_output_rule(mine, m) != want) {
			WB_CHECK(false);
			break;
		}
	}
}

WB_TEST(output_rules_written_in_place) {
	auto doc = RcDocument::from_string(kRulesRc);
	WayboxSettings before = doc->read(), s = before;
	/* The serial rule edited where it is. */
	s.outputs[0].attrs["scale"] = "1.5";
	/* The name rule turned into one following the monitor (make, model,
	 * serial): rewritten in place, so it stays second. */
	s.outputs[1].name.clear();
	s.outputs[1].make = "LG";
	s.outputs[1].model = "27GL";
	s.outputs[1].serial = "9";
	/* The make+model rule's monitor (one of two) gets its own rule, before
	 * the shared one, so it wins. */
	wb::OutputSetting mine;
	mine.name = "eDP-1";
	mine.attrs = {{"scale", "1.25"}};
	mine.before = s.outputs[2].from;
	s.outputs.push_back(mine);
	/* HDMI-A-1's rule back to automatic: it goes (and its odd attribute). */
	s.outputs[3].attrs.clear();
	doc->apply(s, before);
	std::string out = doc->to_string();
	size_t serial = out.find("serial=\"5XYZ\" scale=\"1.5\"");
	size_t lg = out.find("make=\"LG\" model=\"27GL\" serial=\"9\"");
	size_t edp = out.find("<output name=\"eDP-1\" scale=\"1.25\"/>");
	size_t boe = out.find("make=\"BOE\"");
	WB_CHECK(serial != std::string::npos && lg != std::string::npos);
	WB_CHECK(edp != std::string::npos && boe != std::string::npos);
	WB_CHECK(serial < lg && lg < edp && edp < boe);           /* order kept */
	WB_CHECK(out.find("name=\"DP-3\"") == std::string::npos);  /* rewritten, not added */
	WB_CHECK(out.find("scale=\"1\" position=\"0,0\"") != std::string::npos);
	WB_CHECK(out.find("HDMI-A-1") == std::string::npos);
	/* Left as they were: the duplicate and the rule matching nothing. */
	WB_CHECK(out.find("serial=\"5XYZ\" scale=\"3\"") != std::string::npos);
	WB_CHECK(out.find("<output enabled=\"no\"/>") != std::string::npos);

	/* Read back: as written; and writing it again changes nothing. */
	auto again = RcDocument::from_string(out);
	WayboxSettings r = again->read();
	WB_CHECK(r.outputs.size() == 4);
	WB_CHECK(r.outputs[1].make == "LG" && r.outputs[1].name.empty() &&
			r.outputs[1].attrs.at("position") == "0,0");
	WB_CHECK(r.outputs[2].name == "eDP-1");
	WB_CHECK(wb::applying_output_rule(r.outputs, {"eDP-1", "BOE", "Twin", "A"}) == 2);
	WB_CHECK(wb::applying_output_rule(r.outputs, {"DP-7", "BOE", "Twin", "B"}) == 3);
	again->apply(r, r);
	WB_CHECK(again->to_string() == out);
}

WB_TEST(output_rules_back_and_forth) {
	/* Following a monitor, then not: the same place, nothing doubled. */
	auto doc = RcDocument::from_string(kRulesRc);
	WayboxSettings a = doc->read(), b = a;
	b.outputs[1].name.clear();
	b.outputs[1].serial = "9";
	doc->apply(b, a);
	WayboxSettings c = doc->read(), d = c;
	WB_CHECK(c.outputs[1].serial == "9" && c.outputs[1].from.serial == "9");
	d.outputs[1].serial.clear();
	d.outputs[1].name = "DP-3";
	doc->apply(d, c);
	auto first = RcDocument::from_string(kRulesRc);
	WB_CHECK(doc->to_string() == first->to_string());
	/* A matcher emptied altogether: the rule goes (it would match nothing). */
	WayboxSettings e = doc->read(), f = e;
	f.outputs[1].name.clear();
	doc->apply(f, e);
	WB_CHECK(doc->to_string().find("position=\"0,0\"") == std::string::npos);
}

WB_TEST(output_snapshot_and_restore) {
	auto doc = RcDocument::from_string(kRulesRc);
	std::string outputs = doc->snapshot({"waybox", "outputs"});
	std::string hdr = doc->snapshot({"waybox", "hdr"});
	WB_CHECK(outputs.find("<!--") == std::string::npos && outputs.find("5XYZ") != std::string::npos);
	WB_CHECK(hdr.empty());
	std::string original = doc->to_string();
	WayboxSettings a = doc->read(), b = a;
	b.outputs.clear();
	b.hdr_outputs = "DP-3";
	doc->apply(b, a);
	WB_CHECK(doc->to_string() != original);
	doc->restore({"waybox", "outputs"}, outputs);
	doc->restore({"waybox", "hdr"}, hdr);
	auto back = RcDocument::from_string(doc->to_string());
	WB_CHECK(back.has_value());
	WB_CHECK(back->read().outputs == a.outputs);
	WB_CHECK(!back->read().hdr_outputs.has_value());
	/* Into a file without them: added; not what was saved: ignored. */
	auto bare = RcDocument::from_string("<?xml version=\"1.0\"?>\n"
			"<openbox_config xmlns=\"http://openbox.org/3.4/rc\"/>\n");
	bare->restore({"waybox", "outputs"}, outputs);
	WB_CHECK(bare->read().outputs == a.outputs);
	std::string before_junk = bare->to_string();
	bare->restore({"waybox", "outputs"}, "<hdr outputs=\"x\"/>");
	bare->restore({"waybox", "outputs"}, "not xml <<<");
	WB_CHECK(bare->to_string() == before_junk);
}

WB_TEST(output_rules_random_edits) {
	/* Random rules and random edits: always valid XML, edits read back as
	 * meant, untouched rules kept, a second write a no-op. */
	std::mt19937 rng(2350);
	const char *vals[] = {"", "DP-1", "DP-2", "Dell Inc.", "\xc3\xa9\xe6\x97\xa5", "a&b",
			"q\"uote", "<x>", "  sp  "};
	auto val = [&] { return std::string(vals[rng() % 9]); };
	auto esc = [](const std::string &v) {
		std::string o;
		for (char c : v)
			o += c == '&' ? "&amp;" : c == '"' ? "&quot;" : c == '<' ? "&lt;" : std::string(1, c);
		return o;
	};
	for (int round = 0; round < 1500; ++round) {
		std::string xml = "<?xml version=\"1.0\"?>\n<openbox_config xmlns=\"http://openbox.org/3.4/rc\">"
				"<waybox><outputs>\n";
		int n = static_cast<int>(rng() % 6);
		for (int i = 0; i < n; ++i) {
			xml += "<output";
			for (const char *k : {"name", "make", "model", "serial"})
				if (rng() % 2)
					xml += std::string(" ") + k + "=\"" + esc(val()) + "\"";
			if (rng() % 2)
				xml += " scale=\"" + std::to_string(1 + rng() % 3) + "\"";
			if (rng() % 3 == 0)
				xml += " note=\"" + esc(val()) + "\"";
			xml += "/>\n";
		}
		xml += "</outputs></waybox></openbox_config>\n";
		auto doc = RcDocument::from_string(xml);
		if (!doc) {
			WB_CHECK(false);
			break;
		}
		WayboxSettings a = doc->read(), b = a;
		std::vector<bool> touched(b.outputs.size(), false);
		for (size_t i = 0; i < b.outputs.size(); ++i) {
			switch (rng() % 5) {
			case 0:  /* rematched */
				b.outputs[i].name = val();
				b.outputs[i].serial = val();
				touched[i] = true;
				break;
			case 1:  /* back to automatic */
				b.outputs[i].attrs.clear();
				touched[i] = true;
				break;
			case 2:
				b.outputs[i].attrs["scale"] = "1.5";
				touched[i] = true;
				break;
			default:
				break;
			}
		}
		if (rng() % 2) {
			wb::OutputSetting fresh;
			fresh.name = val();
			fresh.attrs = {{"scale", "2"}};
			if (!b.outputs.empty())
				fresh.before = b.outputs[rng() % b.outputs.size()].from;
			b.outputs.push_back(fresh);
		}
		doc->apply(b, a);
		std::string out = doc->to_string();
		auto again = RcDocument::from_string(out);
		if (!again) {
			WB_CHECK(false);
			break;
		}
		WayboxSettings r = again->read();
		/* Every rule meant to be there, with its attributes. */
		bool ok = true;
		for (size_t i = 0; i < b.outputs.size(); ++i) {
			const wb::OutputSetting &want = b.outputs[i];
			bool gone = want.attrs.empty() || want.match().empty();
			bool found = false;
			for (const wb::OutputSetting &got : r.outputs)
				found = found || (got.match() == want.match() && got.attrs == want.attrs);
			/* A rule whose new matcher an earlier rule has already isn't
			 * read (it never applies): only check those that can apply. */
			bool shadowed = false;
			for (size_t j = 0; j < b.outputs.size(); ++j)
				if (j != i && b.outputs[j].match() == want.match())
					shadowed = true;
			if (!gone && !shadowed && !found)
				ok = false;
		}
		/* Untouched rules: still read, as they were. */
		for (size_t i = 0; i < a.outputs.size(); ++i)
			if (!touched[i]) {
				bool found = false;
				for (const wb::OutputSetting &got : r.outputs)
					found = found || (got.match() == a.outputs[i].match() &&
							got.attrs == a.outputs[i].attrs);
				bool shadowed = false;
				for (size_t j = 0; j < b.outputs.size(); ++j)
					if (j >= a.outputs.size() || touched[j])
						shadowed = shadowed || b.outputs[j].match() == a.outputs[i].match();
				if (!found && !shadowed)
					ok = false;
			}
		/* Writing what was read changes nothing. */
		again->apply(r, r);
		if (!ok || again->to_string() != out) {
			WB_CHECK(false);
			fprintf(stderr, "round %d:\n%s\n->\n%s\n", round, xml.c_str(), out.c_str());
			break;
		}
	}
}

/* Bindings: read (chains flattened), edited in place (comments kept),
 * added (a chain's keys reused), removed (an emptied chain key too);
 * advanced ones (If) left to rc.xml. */
WB_TEST(bindings_read_and_edit) {
	const char *xml = R"(<?xml version="1.0"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <keyboard>
    <!-- keep me -->
    <keybind key="W-e"><action name="Execute"><command>foot</command></action></keybind>
    <keybind key="C-a">
      <keybind key="x"><action name="Close"/></keybind>
    </keybind>
    <keybind key="W-i"><action name="If"><then><action name="Raise"/></then></action></keybind>
  </keyboard>
  <mouse>
    <context name="Root">
      <mousebind button="Right" action="Press"><action name="ShowMenu"><menu>root-menu</menu></action></mousebind>
    </context>
  </mouse>
</openbox_config>
)";
	auto doc = wb::RcDocument::from_string(xml);
	WB_CHECK(doc.has_value());
	if (!doc)
		return;
	auto keys = doc->keybindings();
	WB_CHECK(keys.size() == 3);
	WB_CHECK((keys[0].chain == std::vector<std::string>{"W-e"}));
	WB_CHECK(keys[0].actions.size() == 1 && keys[0].actions[0].name == "Execute" &&
			keys[0].actions[0].options[0] == std::pair<std::string, std::string>("command", "foot"));
	WB_CHECK((keys[1].chain == std::vector<std::string>{"C-a", "x"}));
	WB_CHECK(keys[2].actions.size() == 1 && keys[2].actions[0].advanced);
	auto mouse = doc->mousebindings();
	WB_CHECK(mouse.size() == 1 && mouse[0].context == "Root" && mouse[0].button == "Right" &&
			mouse[0].event == "Press" && mouse[0].actions[0].name == "ShowMenu");

	/* In place: W-e now runs alacritty; the comment stays. */
	wb::KeyBinding e = keys[0];
	e.actions[0].options = {{"command", "alacritty"}};
	WB_CHECK(doc->set_keybinding({"W-e"}, e));
	std::string out = doc->to_string();
	WB_CHECK(out.find("keep me") != std::string::npos);
	WB_CHECK(out.find("alacritty") != std::string::npos && out.find(">foot<") == std::string::npos);
	/* Repeat while held: written as repeat="yes", read back; off, taken
	 * away (an explicit "no" of the user's kept). */
	WB_CHECK(!keys[0].repeat);
	e.repeat = true;
	WB_CHECK(doc->set_keybinding({"W-e"}, e));
	WB_CHECK(doc->to_string().find("key=\"W-e\" repeat=\"yes\"") != std::string::npos);
	WB_CHECK(doc->keybindings()[0].repeat);
	e.repeat = false;
	WB_CHECK(doc->set_keybinding({"W-e"}, e));
	WB_CHECK(doc->to_string().find("repeat=") == std::string::npos);
	{
		auto no = wb::RcDocument::from_string(R"(<openbox_config><keyboard>
<keybind key="F7" repeat="no"><action name="Close"/></keybind></keyboard></openbox_config>)");
		WB_CHECK(no.has_value() && !no->keybindings()[0].repeat);
		wb::KeyBinding f7 = no->keybindings()[0];
		f7.actions = {{"Iconify", {}}};
		WB_CHECK(no->set_keybinding({"F7"}, f7));
		WB_CHECK(no->to_string().find("repeat=\"no\"") != std::string::npos);
		/* A new one, repeating: on its own <keybind>. */
		WB_CHECK(no->set_keybinding({}, wb::KeyBinding{{"XF86AudioRaiseVolume"},
				{{"Execute", {{"command", "wpctl set-volume @DEFAULT_SINK@ 5%+"}}}}, true}));
		WB_CHECK(no->to_string().find("key=\"XF86AudioRaiseVolume\" repeat=\"yes\"") !=
				std::string::npos);
	}
	/* Added under the existing C-a. */
	WB_CHECK(doc->set_keybinding({}, wb::KeyBinding{{"C-a", "y"}, {{"Iconify", {}}}}));
	keys = doc->keybindings();
	WB_CHECK(keys.size() == 4 && (keys[2].chain == std::vector<std::string>{"C-a", "y"}));
	/* Removed: C-a x, then C-a y; C-a itself goes with the last. */
	/* Added on keys another binding has: both kept, side by side (the
	 * other's actions not overwritten). */
	WB_CHECK(doc->set_keybinding({}, wb::KeyBinding{{"W-e"}, {{"Close", {}}}}));
	keys = doc->keybindings();
	WB_CHECK(std::count_if(keys.begin(), keys.end(), [](const wb::KeyBinding &k) {
		return k.chain == std::vector<std::string>{"W-e"};
	}) == 2);
	WB_CHECK(doc->to_string().find("alacritty") != std::string::npos);
	/* Which of the two: told by what it ran. */
	std::vector<wb::BoundAction> ran_alacritty = keys[0].actions;
	WB_CHECK(doc->set_keybinding({"W-e"}, std::nullopt, &ran_alacritty));
	keys = doc->keybindings();
	WB_CHECK(std::any_of(keys.begin(), keys.end(), [](const wb::KeyBinding &k) {
		return k.chain == std::vector<std::string>{"W-e"} && k.actions[0].name == "Close";
	}));
	WB_CHECK(doc->set_keybinding({"C-a", "x"}, std::nullopt));
	WB_CHECK(doc->set_keybinding({"C-a", "y"}, std::nullopt));
	WB_CHECK(doc->to_string().find("C-a") == std::string::npos);
	/* Advanced: refused, left as it is. */
	WB_CHECK(!doc->set_keybinding({"W-i"}, std::nullopt));
	WB_CHECK(doc->to_string().find("<then>") != std::string::npos);
	/* Mouse: moved to another button (removed, added), then removed. */
	wb::MouseBinding m = mouse[0];
	wb::MouseBinding moved = m;
	moved.button = "Middle";
	WB_CHECK(doc->set_mousebinding(m, moved));
	mouse = doc->mousebindings();
	WB_CHECK(mouse.size() == 1 && mouse[0].button == "Middle");
	WB_CHECK(doc->set_mousebinding(moved, std::nullopt));
	WB_CHECK(doc->mousebindings().empty());
	/* A new context made for one. */
	WB_CHECK(doc->set_mousebinding(std::nullopt,
			wb::MouseBinding{"Titlebar", "Left", "DoubleClick", {{"ToggleMaximize", {}}}}));
	WB_CHECK(doc->mousebindings().size() == 1);
}

/* Adding never merges into another binding, and finding one never takes a
 * chain's start for it (review of #236). */
WB_TEST(bindings_kept_apart) {
	auto chains = [](const wb::RcDocument &d) {
		std::vector<std::string> out;
		for (const wb::KeyBinding &k : d.keybindings()) {
			std::string c;
			for (const std::string &s : k.chain)
				c += (c.empty() ? "" : " ") + s;
			out.push_back(c + "=" + (k.actions.empty() ? "" : k.actions[0].name));
		}
		return out;
	};
	using V = std::vector<std::string>;
	/* A single key, then a chain starting with it: the leaf stays one. */
	auto doc = wb::RcDocument::from_string(
			"<openbox_config xmlns=\"http://openbox.org/3.4/rc\"><keyboard>"
			"<keybind key=\"C-a\"><action name=\"Close\"/></keybind>"
			"</keyboard></openbox_config>");
	WB_CHECK(doc.has_value());
	if (!doc)
		return;
	WB_CHECK(doc->set_keybinding({}, wb::KeyBinding{{"C-a", "x"}, {{"Iconify", {}}}}));
	WB_CHECK(chains(*doc) == (V{"C-a=Close", "C-a x=Iconify"}));
	/* A chain, then the single key: its own <keybind>, listed. */
	doc = wb::RcDocument::from_string(
			"<openbox_config xmlns=\"http://openbox.org/3.4/rc\"><keyboard>"
			"<keybind key=\"C-a\"><keybind key=\"x\"><action name=\"Close\"/></keybind></keybind>"
			"</keyboard></openbox_config>");
	WB_CHECK(doc.has_value());
	if (!doc)
		return;
	WB_CHECK(doc->set_keybinding({}, wb::KeyBinding{{"C-a"}, {{"Iconify", {}}}}));
	WB_CHECK(chains(*doc) == (V{"C-a x=Close", "C-a=Iconify"}));
	/* A second chain under the same start: shares it. */
	WB_CHECK(doc->set_keybinding({}, wb::KeyBinding{{"C-a", "y"}, {{"Raise", {}}}}));
	WB_CHECK(doc->to_string().find("key=\"C-a\"") != std::string::npos);
	size_t first = doc->to_string().find("key=\"C-a\"");
	WB_CHECK(doc->to_string().find("key=\"y\"") > first);
	/* Removing the single key leaves the chain. */
	std::vector<wb::BoundAction> iconify = {{"Iconify", {}}};
	WB_CHECK(doc->set_keybinding({"C-a"}, std::nullopt, &iconify));
	WB_CHECK(chains(*doc) == (V{"C-a x=Close", "C-a y=Raise"}));

	/* An empty single key beside a chain: removing it can't take the
	 * chain's start for it. */
	doc = wb::RcDocument::from_string(
			"<openbox_config xmlns=\"http://openbox.org/3.4/rc\"><keyboard>"
			"<keybind key=\"C-a\"><keybind key=\"x\"><action name=\"Close\"/></keybind></keybind>"
			"<keybind key=\"C-a\"/>"
			"</keyboard></openbox_config>");
	WB_CHECK(doc.has_value());
	if (!doc)
		return;
	std::vector<wb::BoundAction> none;
	WB_CHECK(doc->set_keybinding({"C-a"}, std::nullopt, &none));
	WB_CHECK(chains(*doc) == (V{"C-a x=Close"}));
	WB_CHECK(doc->set_keybinding({"C-a"}, std::nullopt) == false ||
			chains(*doc) == (V{"C-a x=Close"}));

	/* Two mouse bindings on the same place: the one meant is removed. */
	doc = wb::RcDocument::from_string(
			"<openbox_config xmlns=\"http://openbox.org/3.4/rc\"><mouse><context name=\"Root\">"
			"<mousebind button=\"Right\" action=\"Press\"><action name=\"ShowMenu\"/></mousebind>"
			"<mousebind button=\"Right\" action=\"Press\"><action name=\"Close\"/></mousebind>"
			"</context></mouse></openbox_config>");
	WB_CHECK(doc.has_value());
	if (!doc)
		return;
	auto mouse = doc->mousebindings();
	WB_CHECK(mouse.size() == 2);
	WB_CHECK(doc->set_mousebinding(mouse[1], std::nullopt));
	mouse = doc->mousebindings();
	WB_CHECK(mouse.size() == 1 && mouse[0].actions[0].name == "ShowMenu");
}

/* Window rules (#237): read as Waybox reads them; edited, added, moved and
 * removed in place, what Waybox doesn't read kept as written. */
WB_TEST(app_rules_read_and_edit) {
	auto doc = wb::RcDocument::from_string(R"(<?xml version="1.0"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <applications>
    <!-- videos -->
    <application class="mpv">
      <fullscreen>yes</fullscreen>
    </application>
    <application class="foot" title="*scratch*" role="scratch">
      <position><x>200</x><y>120</y></position>
      <size><width>900</width><height>600</height></size>
      <decor>no</decor>
      <shade>yes</shade>
    </application>
    <application class="gimp">
      <position force="yes"><x>center</x><y>left</y></position>
      <desktop>2</desktop>
    </application>
  </applications>
</openbox_config>
)");
	WB_CHECK(doc.has_value());
	if (!doc)
		return;
	auto rules = doc->app_rules();
	WB_CHECK(rules.size() == 3);
	if (rules.size() != 3)
		return;
	WB_CHECK(rules[0].rule.class_pattern == "mpv" && rules[0].rule.fullscreen == true);
	WB_CHECK(rules[1].rule.title_pattern == "*scratch*" && rules[1].rule.x == 200 &&
			rules[1].rule.height == 600 && rules[1].rule.decor == false);
	WB_CHECK((rules[1].kept == std::vector<std::string>{"role", "shade"}));
	WB_CHECK(!rules[1].position_kept);
	/* "center" read (Openbox's form); "left" isn't one: the position kept. */
	WB_CHECK(rules[2].position_kept && rules[2].rule.desktop == 1 &&
			rules[2].rule.x_spec == "center");

	/* Edited in place: what it sets written, the rest kept. */
	wb::AppRule foot = rules[1].rule;
	foot.decor = true;
	foot.desktop = wb::AppRule::kAllDesktops;
	foot.x.reset();
	foot.width.reset();
	foot.height.reset();
	WB_CHECK(doc->set_app_rule(1, foot));
	std::string out = doc->to_string();
	WB_CHECK(out.find("<!-- videos -->") != std::string::npos);
	WB_CHECK(out.find("role=\"scratch\"") != std::string::npos);
	WB_CHECK(out.find("<shade>yes</shade>") != std::string::npos);
	WB_CHECK(out.find("<decor>yes</decor>") != std::string::npos);
	WB_CHECK(out.find("<desktop>all</desktop>") != std::string::npos);
	WB_CHECK(out.find("<x>200</x>") == std::string::npos && out.find("<y>120</y>") != std::string::npos);
	WB_CHECK(out.find("<size>") == std::string::npos);
	/* A position Waybox reads only in part: never rewritten. */
	wb::AppRule gimp = rules[2].rule;
	gimp.desktop.reset();
	WB_CHECK(doc->set_app_rule(2, gimp));
	WB_CHECK(doc->to_string().find("<x>center</x>") != std::string::npos);
	WB_CHECK(doc->to_string().find("<desktop>2</desktop>") == std::string::npos);
	/* Read back as written. */
	rules = doc->app_rules();
	WB_CHECK(rules.size() == 3 && rules[1].rule == foot);

	/* Added at the end, on lines of its own. */
	wb::AppRule fresh;
	fresh.class_pattern = "firefox";
	fresh.maximized = true;
	WB_CHECK(doc->set_app_rule(std::nullopt, fresh));
	rules = doc->app_rules();
	WB_CHECK(rules.size() == 4 && rules[3].rule == fresh);
	WB_CHECK(doc->to_string().find("\n    <application class=\"firefox\">\n      <maximized>yes"
			"</maximized>\n    </application>") != std::string::npos);

	/* Moved: firefox first, then back to the end. */
	WB_CHECK(doc->move_app_rule(3, 0));
	rules = doc->app_rules();
	WB_CHECK(rules[0].rule.class_pattern == "firefox" && rules[1].rule.class_pattern == "mpv");
	WB_CHECK(doc->move_app_rule(0, 3));
	rules = doc->app_rules();
	WB_CHECK(rules[3].rule.class_pattern == "firefox" && rules[0].rule.class_pattern == "mpv");
	WB_CHECK(!doc->move_app_rule(0, 9));

	/* Removed. */
	WB_CHECK(doc->set_app_rule(0, std::nullopt));
	rules = doc->app_rules();
	WB_CHECK(rules.size() == 3 && rules[0].rule.class_pattern == "foot");
	WB_CHECK(!doc->set_app_rule(7, std::nullopt));

	/* Regex patterns: read, written, not counted as kept. */
	auto rx = wb::RcDocument::from_string(
			"<openbox_config xmlns=\"http://openbox.org/3.4/rc\"><applications>"
			"<application match=\"iregex\" class=\"^foot$\"><decor>no</decor></application>"
			"</applications></openbox_config>");
	WB_CHECK(rx.has_value());
	if (rx) {
		auto r = rx->app_rules();
		WB_CHECK(r.size() == 1 && r[0].rule.patterns == wb::AppRule::Patterns::IRegex &&
				r[0].kept.empty());
		wb::AppRule glob = r[0].rule;
		glob.patterns = wb::AppRule::Patterns::Glob;
		WB_CHECK(rx->set_app_rule(0, glob));
		WB_CHECK(rx->to_string().find("match=") == std::string::npos);
		glob.patterns = wb::AppRule::Patterns::Regex;
		WB_CHECK(rx->set_app_rule(0, glob));
		WB_CHECK(rx->to_string().find("match=\"regex\"") != std::string::npos);
	}

	/* Openbox's positions and a monitor: read, and written back. */
	auto pos = wb::RcDocument::from_string(
			"<openbox_config xmlns=\"http://openbox.org/3.4/rc\"><applications>"
			"<application class=\"mpv\"><position force=\"yes\"><x>center</x><y>-10%</y>"
			"<monitor>2</monitor></position></application>"
			"</applications></openbox_config>");
	WB_CHECK(pos.has_value());
	if (pos) {
		auto r = pos->app_rules();
		/* (A minus: from the far edge, as Openbox reads it.) */
		WB_CHECK(r.size() == 1 && !r[0].position_kept && r[0].rule.x_spec == "center" &&
				r[0].rule.y_spec == "-10%" && r[0].rule.monitor == "2" && !r[0].rule.x);
		wb::AppRule m = r[0].rule;
		m.x_spec.clear();
		m.x = 30;
		m.monitor = "mouse";
		WB_CHECK(pos->set_app_rule(0, m));
		std::string out = pos->to_string();
		WB_CHECK(out.find("<x>30</x>") != std::string::npos &&
				out.find("<y>-10%</y>") != std::string::npos &&
				out.find("<monitor>mouse</monitor>") != std::string::npos);
		WB_CHECK(pos->app_rules()[0].rule == m);
	}

	/* A file with none: <applications> made for the first. */
	auto empty = wb::RcDocument::from_string(
			"<openbox_config xmlns=\"http://openbox.org/3.4/rc\"></openbox_config>");
	WB_CHECK(empty.has_value());
	if (empty) {
		WB_CHECK(empty->app_rules().empty());
		WB_CHECK(empty->set_app_rule(std::nullopt, fresh));
		WB_CHECK(empty->app_rules().size() == 1);
	}
}
