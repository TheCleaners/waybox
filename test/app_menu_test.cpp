#include "wb_test.hpp"

#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "waybox/app_menu.hpp"

using wb::DesktopEntry;

namespace {

DesktopEntry parse(const std::string &text, const std::vector<std::string> &langs = {}) {
	DesktopEntry e;
	WB_CHECK(wb::parse_desktop_entry(text, langs, e));
	return e;
}

void write_file(const std::string &path, const std::string &text) {
	std::ofstream(path) << text;
}

std::string temp_dir() {
	char tmpl[] = "/tmp/wb-app-menu-XXXXXX";
	char *d = mkdtemp(tmpl);
	WB_CHECK(d != nullptr);
	return d;
}

}  // namespace

WB_TEST(locale_names_best_first) {
	auto n = wb::locale_names("de_DE.UTF-8@euro");
	WB_CHECK(n.size() == 4);
	WB_CHECK(n[0] == "de_DE@euro" && n[1] == "de_DE" && n[2] == "de@euro" && n[3] == "de");
	n = wb::locale_names("fr_CA.UTF-8");
	WB_CHECK(n.size() == 2 && n[0] == "fr_CA" && n[1] == "fr");
	WB_CHECK(wb::locale_names("C.UTF-8").empty());
	WB_CHECK(wb::locale_names("").empty());
}

WB_TEST(parse_fields) {
	DesktopEntry e = parse(R"(# comment
[Desktop Entry]
Type=Application
Name=Files
Name[de]=Dateien
Name[de_DE]=Dateien (DE)
Exec=nautilus --new-window %U
Icon=org.gnome.Nautilus
Categories=GNOME;GTK;Utility;Core;FileManager;
Terminal=false
Path=/srv/my dir
TryExec=nautilus

[Desktop Action new-window]
Name=New Window
Exec=nautilus --other
)", wb::locale_names("de_DE.UTF-8"));
	WB_CHECK(e.application);
	WB_CHECK(e.name == "Dateien (DE)");
	WB_CHECK(e.exec == "nautilus --new-window");  /* not the action's */
	WB_CHECK(e.icon == "org.gnome.Nautilus");
	WB_CHECK(e.categories.size() == 5 && e.categories[2] == "Utility");
	WB_CHECK(e.path == "/srv/my dir");
	WB_CHECK(e.try_exec == "nautilus");
	WB_CHECK(!e.terminal);
	/* Without a matching locale: the plain Name. */
	WB_CHECK(parse("[Desktop Entry]\nName[de]=Dateien\nName=Files\n",
			wb::locale_names("fr_FR")).name == "Files");
}

WB_TEST(parse_escapes_and_field_codes) {
	DesktopEntry e = parse("[Desktop Entry]\nType=Application\nName=A\\sB\n"
			"Exec=sh -c \"echo 100%% done\" %f\nCategories=Office\\;Calc;Game;\n");
	WB_CHECK(e.name == "A B");
	WB_CHECK(e.exec == "sh -c \"echo 100% done\"");
	WB_CHECK(e.categories.size() == 2 && e.categories[0] == "Office;Calc");
	DesktopEntry none;
	WB_CHECK(!wb::parse_desktop_entry("[Other]\nName=x\n", {}, none));
}

WB_TEST(shown_rules) {
	std::vector<std::string> here = {"Waybox", "wlroots"};
	DesktopEntry e = parse("[Desktop Entry]\nType=Application\nName=A\nExec=a\n");
	WB_CHECK(wb::desktop_entry_shown(e, here));
	DesktopEntry nd = e;
	nd.no_display = true;
	WB_CHECK(!wb::desktop_entry_shown(nd, here));
	DesktopEntry hidden = e;
	hidden.hidden = true;
	WB_CHECK(!wb::desktop_entry_shown(hidden, here));
	DesktopEntry link = parse("[Desktop Entry]\nType=Link\nName=A\nURL=x\n");
	WB_CHECK(!wb::desktop_entry_shown(link, here));
	DesktopEntry noexec = parse("[Desktop Entry]\nType=Application\nName=A\n");
	WB_CHECK(!wb::desktop_entry_shown(noexec, here));
	DesktopEntry gnome = e;
	gnome.only_show_in = {"GNOME"};
	WB_CHECK(!wb::desktop_entry_shown(gnome, here));
	gnome.only_show_in = {"GNOME", "wlroots"};
	WB_CHECK(wb::desktop_entry_shown(gnome, here));
	DesktopEntry not_here = e;
	not_here.not_show_in = {"Waybox"};
	WB_CHECK(!wb::desktop_entry_shown(not_here, here));
}

WB_TEST(main_category_first_main_one) {
	DesktopEntry e;
	e.categories = {"GTK", "Utility", "Development"};
	WB_CHECK(wb::main_category(e) == "Utility");
	e.categories = {"Qt", "Audio", "Player"};
	WB_CHECK(wb::main_category(e) == "AudioVideo");
	e.categories = {"X-Custom"};
	WB_CHECK(wb::main_category(e) == "Other");
	e.categories = {};
	WB_CHECK(wb::main_category(e) == "Other");
}

WB_TEST(command_terminal_and_path) {
	DesktopEntry e;
	e.exec = "htop";
	WB_CHECK(wb::desktop_entry_command(e, "foot") == "htop");
	e.terminal = true;
	WB_CHECK(wb::desktop_entry_command(e, "foot") == "foot -e htop");
	e.path = "/tmp/it's";
	WB_CHECK(wb::desktop_entry_command(e, "foot") == "cd '/tmp/it'\\''s' && foot -e htop");
}

WB_TEST(build_groups_and_sorts) {
	auto entry = [](const char *id, const char *name, std::vector<std::string> cats) {
		DesktopEntry e;
		e.id = id;
		e.name = name;
		e.exec = std::string(id).substr(0, std::string(id).find('.'));
		e.categories = std::move(cats);
		return e;
	};
	std::vector<DesktopEntry> entries = {
		entry("zed.desktop", "zed", {"Development"}),
		entry("vim.desktop", "Vim", {"Utility", "TextEditor"}),
		entry("gimp.desktop", "GIMP", {"Graphics"}),
		entry("odd.desktop", "Odd", {}),
		entry("code.desktop", "Code", {"Development"}),
		entry("aaa.desktop", "Zebra", {"Development"}),  /* by name, not file */
		entry("calc.desktop", "calculator", {"Utility"}),
	};
	wb::MenuFile f = wb::build_applications_menu("applications", "Applications", entries, "foot");
	const wb::Menu *root = f.find("applications");
	WB_CHECK(root != nullptr && root->label == "Applications");
	/* By label, Other last: Accessories, Development, Graphics, Other. */
	WB_CHECK(root->items.size() == 4);
	WB_CHECK(root->items[0].label == "Accessories" &&
			root->items[0].submenu_id == "applications/Utility");
	WB_CHECK(root->items[1].label == "Development");
	WB_CHECK(root->items[2].label == "Graphics");
	WB_CHECK(root->items[3].label == "Other");
	WB_CHECK(root->items[0].icon == "applications-accessories");
	const wb::Menu *dev = f.find("applications/Development");
	WB_CHECK(dev != nullptr && dev->items.size() == 3);
	WB_CHECK(dev->items[0].label == "Code" && dev->items[1].label == "Zebra" &&
			dev->items[2].label == "zed");
	WB_CHECK(dev->items[0].actions.size() == 1);
	WB_CHECK(dev->items[0].actions[0].type == wb::ActionType::Execute);
	WB_CHECK(dev->items[0].actions[0].command == "code");
	const wb::Menu *acc = f.find("applications/Utility");
	WB_CHECK(acc->items[0].label == "calculator" && acc->items[1].label == "Vim");
	/* Nothing installed: an empty menu, no categories. */
	wb::MenuFile empty = wb::build_applications_menu("a", "A", {}, "foot");
	WB_CHECK(empty.find("a") != nullptr && empty.find("a")->items.empty());
}

WB_TEST(application_dirs_order) {
	auto d = wb::application_dirs("/h/data", "/home/u", "/usr/local/share/:/usr/share:/usr/share");
	WB_CHECK(d.size() == 3);
	WB_CHECK(d[0] == "/h/data/applications");
	WB_CHECK(d[1] == "/usr/local/share/applications");
	WB_CHECK(d[2] == "/usr/share/applications");
	d = wb::application_dirs(nullptr, "/home/u", nullptr);
	WB_CHECK(d.size() == 3 && d[0] == "/home/u/.local/share/applications");
	WB_CHECK(d[2] == "/usr/share/applications");
	/* Relative directories don't count (the spec). */
	d = wb::application_dirs("rel", nullptr, "rel2:/opt/share");
	WB_CHECK(d.size() == 1 && d[0] == "/opt/share/applications");
}

WB_TEST(scan_shadowing_subdirs_tryexec) {
	std::string root = temp_dir();
	std::string user = root + "/user", sys = root + "/sys";
	mkdir(user.c_str(), 0755);
	mkdir(sys.c_str(), 0755);
	mkdir((sys + "/kde").c_str(), 0755);
	const char *app = "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\n";
	auto entry = [app](const char *name, const char *exec, const char *extra = "") {
		char buf[256];
		snprintf(buf, sizeof buf, app, name, exec);
		return std::string(buf) + extra;
	};
	write_file(sys + "/a.desktop", entry("A system", "a"));
	write_file(user + "/a.desktop", entry("A mine", "a"));
	write_file(sys + "/b.desktop", entry("B", "b"));
	write_file(user + "/b.desktop", entry("B", "b", "Hidden=true\n"));  /* hides the system one */
	write_file(sys + "/kde/c.desktop", entry("C", "c"));
	write_file(sys + "/d.desktop", entry("D", "d", "TryExec=/nonexistent/prog\n"));
	write_file(sys + "/e.desktop", entry("E", "e", "TryExec=/bin/sh\n"));
	write_file(sys + "/notes.txt", "not an entry");
	auto found = wb::scan_applications({user, sys}, {}, {"Waybox"});
	std::vector<std::string> ids;
	for (const DesktopEntry &e : found)
		ids.push_back(e.id + "=" + e.name);
	WB_CHECK(ids.size() == 3);
	WB_CHECK(ids[0] == "a.desktop=A mine");
	WB_CHECK(ids[1] == "e.desktop=E");
	WB_CHECK(ids[2] == "kde-c.desktop=C");

	/* The cache: the same menu until a directory changes. */
	wb::ApplicationsMenu cache;
	const wb::MenuFile &first = cache.get("apps", "Apps", {user, sys}, {}, {"Waybox"}, "foot");
	const wb::Menu *other = first.find("apps/Other");
	WB_CHECK(other != nullptr && other->items.size() == 3);
	/* A new file; the directory's mtime must move (some filesystems keep
	 * whole seconds). */
	sleep(1);
	write_file(user + "/f.desktop", entry("F", "f"));
	const wb::MenuFile &second = cache.get("apps", "Apps", {user, sys}, {}, {"Waybox"}, "foot");
	WB_CHECK(second.find("apps/Other")->items.size() == 4);
	/* Asked for another terminal: built again. */
	WB_CHECK(cache.get("apps", "Apps", {user, sys}, {}, {"Waybox"}, "kitty")
			.find("apps/Other")->items.size() == 4);
	std::string rm = "rm -rf '" + root + "'";
	WB_CHECK(system(rm.c_str()) == 0);
}
