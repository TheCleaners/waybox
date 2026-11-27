#include "waybox/conditions.hpp"

#include "waybox/applications.hpp"

namespace wb {

TextMatch make_text_match(std::string_view kind, std::string pattern) {
	TextMatch m;
	m.set = true;
	m.pattern = std::move(pattern);
	if (kind == "regex") {
		m.kind = TextMatch::Kind::Regex;
		try {
			m.regex = std::make_shared<const std::regex>(m.pattern, std::regex::ECMAScript);
		} catch (const std::regex_error &) {
			m.regex = nullptr;  /* matches nothing */
		}
	} else if (kind == "exact") {
		m.kind = TextMatch::Kind::Exact;
	}
	return m;
}

bool text_matches(const TextMatch &m, std::string_view text) {
	if (!m.set)
		return true;
	switch (m.kind) {
	case TextMatch::Kind::Exact:
		return text == m.pattern;
	case TextMatch::Kind::Regex:
		return m.regex != nullptr &&
				std::regex_search(text.begin(), text.end(), *m.regex);
	case TextMatch::Kind::Glob:
	default:
		return glob_match(m.pattern, text);
	}
}

namespace {

bool holds(const std::optional<bool> &want, bool have) {
	return !want || *want == have;
}

bool query_true(const Query &q, const WindowFacts *w, const DesktopFacts &d) {
	if (q.active_desktop != 0 && d.current != q.active_desktop - 1)
		return false;
	/* No window: false, unless the query asked about the desktop shown,
	 * which held (as Openbox: its other conditions are then passed over). */
	if (w == nullptr)
		return q.active_desktop != 0;
	bool here = w->desktop == d.current || w->omnipresent;
	switch (q.desktop) {
	case Query::Desktop::Current:
		if (!here)
			return false;
		break;
	case Query::Desktop::Other:
		if (here)
			return false;
		break;
	case Query::Desktop::Last:
		if (w->desktop != d.last)
			return false;
		break;
	case Query::Desktop::Number:
		if (w->desktop != q.desktop_number - 1 && !w->omnipresent)
			return false;
		break;
	case Query::Desktop::Any:
		break;
	}
	return holds(q.shaded, w->shaded) &&
			holds(q.maximized, w->maximized_horizontal && w->maximized_vertical) &&
			holds(q.maximized_horizontal, w->maximized_horizontal) &&
			holds(q.maximized_vertical, w->maximized_vertical) &&
			holds(q.iconified, w->iconified) && holds(q.fullscreen, w->fullscreen) &&
			holds(q.focused, w->focused) && holds(q.focusable, w->focusable) &&
			holds(q.urgent, w->urgent) && holds(q.undecorated, !w->decorated) &&
			holds(q.omnipresent, w->omnipresent) &&
			/* Wayland has one name for a program, its app id: Openbox's
			 * class and name both match it; role matches the tag. */
			text_matches(q.title, w->title) && text_matches(q.cls, w->app_id) &&
			text_matches(q.name, w->app_id) && text_matches(q.role, w->tag) &&
			text_matches(q.type, w->type) &&
			(q.monitor == 0 || w->monitor == q.monitor);
}

}  // namespace

bool conditions_true(const std::vector<Query> &queries, const WindowFacts *action_target,
		const WindowFacts *focus_target, const DesktopFacts &desktops) {
	for (const Query &q : queries)
		if (!query_true(q, q.focus_target ? focus_target : action_target, desktops))
			return false;
	return true;
}

}  // namespace wb
