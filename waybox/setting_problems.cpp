#include "waybox/setting_problems.hpp"

#include <fstream>

#include "waybox/xcursor_themes.hpp"

namespace wb {

namespace {

/* "“name”". (Not "quoted": libc++'s std::quoted, found by argument-
 * dependent lookup, wins over it for a non-const string.) */
std::string in_quotes(const std::string &s) {
	return "\xE2\x80\x9C" + s + "\xE2\x80\x9D";
}

std::string replace(std::string s, const std::string &what, const std::string &with) {
	size_t at = s.find(what);
	if (at != std::string::npos)
		s.replace(at, what.size(), with);
	return s;
}

}  // namespace

std::string tilde(const std::string &file, const std::string &home) {
	std::string h = home;
	while (h.size() > 1 && h.back() == '/')
		h.pop_back();
	if (h.empty() || h == "/" || file.compare(0, h.size(), h) != 0 ||
			(file.size() > h.size() && file[h.size()] != '/'))
		return file;
	return "~" + file.substr(h.size());
}

std::string written_by(const std::string &file) {
	std::ifstream in(file);
	std::string line;
	for (int n = 0; n < 5 && std::getline(in, line); ++n) {
		if (line.empty() || (line[0] != '#' && line[0] != ';'))
			continue;
		size_t at = line.find("written by ");
		if (at == std::string::npos)
			continue;
		std::string who = line.substr(at + 11);
		size_t end = who.find_first_of(".,;:!(\r");
		who = who.substr(0, end);
		while (!who.empty() && who.back() == ' ')
			who.pop_back();
		if (!who.empty() && who.size() <= 40)
			return who;
	}
	return "";
}

std::optional<SettingProblem> pointer_problem(const PointerState &s, Translate tr) {
	auto _ = [tr](const char *msg) { return std::string(tr != nullptr ? tr(msg) : msg); };
	/* What Waybox draws now: rc.xml's theme, else what it was started
	 * with, else "default". */
	std::string waybox = s.rc_theme && !s.rc_theme->empty() ? *s.rc_theme
			: !s.env_theme.empty() ? s.env_theme : "default";
	XcursorThemeCheck check = xcursor_theme_check(waybox, s.path);
	bool shown_ok = !s.shown.empty() && xcursor_theme_check(s.shown, s.path).has_cursors;
	SettingProblem p;
	if (!check.has_cursors) {
		std::string instead = xcursor_fallback_theme(s.path);
		p.text = replace(_("Waybox's pointer theme, {theme}, has no cursors"), "{theme}",
				in_quotes(waybox));
		if (!check.index_file.empty()) {
			std::string who = written_by(check.index_file);
			p.text += ": " + (who.empty()
					? replace(_("{file} falls back to no theme that has any"), "{file}",
							tilde(check.index_file, s.home))
					: replace(replace(_("{file}, written by {program}, falls back to no "
							"theme that has any"), "{file}", tilde(check.index_file, s.home)),
							"{program}", who));
		}
		p.text += ". " + (instead.empty()
				? _("Waybox draws a stand-in pointer, whose size can't change.")
				: replace(_("Waybox uses {theme} instead."), "{theme}", in_quotes(instead)));
	} else if (shown_ok && s.shown != waybox) {
		p.text = replace(replace(_("Waybox draws the pointer with {waybox}, GTK programs with "
				"{gtk}: it changes look between them."), "{waybox}", in_quotes(waybox)),
				"{gtk}", in_quotes(s.shown));
	} else {
		return std::nullopt;
	}
	if (s.to_be_written && shown_ok)
		p.fixed_on_save = true;
	else if (shown_ok)
		p.fix_label = replace(_("Use {theme} for Waybox Too"), "{theme}", s.shown);
	return p;
}

}  // namespace wb
