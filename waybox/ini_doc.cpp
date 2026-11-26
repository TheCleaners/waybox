#include "waybox/ini_doc.hpp"

#include <algorithm>

namespace wb::ini {

namespace {

std::string_view trim(std::string_view s) {
	size_t b = s.find_first_not_of(" \t\r");
	if (b == std::string_view::npos)
		return {};
	size_t e = s.find_last_not_of(" \t\r");
	return s.substr(b, e - b + 1);
}

}  // namespace

Document Document::parse(std::string_view text) {
	Document d;
	d.final_newline_ = text.empty() || text.back() == '\n';
	std::string section;
	size_t start = 0;
	while (start < text.size()) {
		size_t nl = text.find('\n', start);
		std::string_view raw = text.substr(start, nl == std::string_view::npos
				? std::string_view::npos : nl - start);
		start = nl == std::string_view::npos ? text.size() : nl + 1;
		Line l;
		l.raw = std::string(raw);
		std::string_view t = trim(raw);
		if (t.empty()) {
			l.kind = Kind::Blank;
		} else if (t[0] == '#' || t[0] == ';') {
			l.kind = Kind::Comment;
		} else if (t.front() == '[' && t.back() == ']') {
			l.kind = Kind::Section;
			section = std::string(trim(t.substr(1, t.size() - 2)));
		} else if (size_t eq = raw.find('='); eq != std::string_view::npos &&
				!trim(raw.substr(0, eq)).empty()) {
			l.kind = Kind::Entry;
			l.key = std::string(trim(raw.substr(0, eq)));
			size_t v = raw.find_first_not_of(" \t", eq + 1);
			l.value_at = v == std::string_view::npos ? raw.size() : v;
			l.value = std::string(trim(raw.substr(eq + 1)));
		}
		l.section = section;
		d.lines_.push_back(std::move(l));
	}
	return d;
}

int Document::last_entry(std::string_view section, std::string_view key) const {
	for (size_t i = lines_.size(); i-- > 0;)
		if (lines_[i].kind == Kind::Entry && lines_[i].section == section &&
				lines_[i].key == key)
			return static_cast<int>(i);
	return -1;
}

std::optional<std::string> Document::get(std::string_view section, std::string_view key) const {
	int i = last_entry(section, key);
	if (i < 0)
		return std::nullopt;
	return lines_[static_cast<size_t>(i)].value;
}

/* Where a new key of `section` goes: after its last entry (or header), or
 * for "" before the first header (and the blank lines above it). npos: the
 * section isn't there. */
size_t Document::insert_at(std::string_view section) const {
	int last = -1;
	for (size_t i = 0; i < lines_.size(); ++i) {
		const Line &l = lines_[i];
		if (l.section != section)
			continue;
		if (l.kind == Kind::Entry || l.kind == Kind::Section)
			last = static_cast<int>(i);
	}
	if (last >= 0)
		return static_cast<size_t>(last) + 1;
	if (section.empty()) {
		/* No global keys yet: before the first section's header and the
		 * blank lines and comments just above it. */
		for (size_t i = 0; i < lines_.size(); ++i)
			if (lines_[i].kind == Kind::Section) {
				size_t at = i;
				while (at > 0 && (lines_[at - 1].kind == Kind::Blank ||
						lines_[at - 1].kind == Kind::Comment))
					--at;
				return at;
			}
		return lines_.size();
	}
	return std::string::npos;
}

void Document::set(std::string_view section, std::string_view key, std::string_view value) {
	int i = last_entry(section, key);
	if (i >= 0) {
		Line &l = lines_[static_cast<size_t>(i)];
		l.raw = l.raw.substr(0, l.value_at) + std::string(value);
		l.value = std::string(value);
		return;
	}
	Line entry;
	entry.kind = Kind::Entry;
	entry.section = std::string(section);
	entry.key = std::string(key);
	entry.value = std::string(value);
	entry.raw = entry.key + "=" + entry.value;
	entry.value_at = entry.key.size() + 1;
	size_t at = insert_at(section);
	if (at != std::string::npos) {
		lines_.insert(lines_.begin() + static_cast<long>(at), std::move(entry));
		return;
	}
	/* A new section at the end, a blank line before it. */
	if (!lines_.empty() && lines_.back().kind != Kind::Blank) {
		Line blank;
		blank.kind = Kind::Blank;
		blank.section = lines_.back().section;
		lines_.push_back(std::move(blank));
	}
	Line head;
	head.kind = Kind::Section;
	head.section = std::string(section);
	head.raw = "[" + std::string(section) + "]";
	lines_.push_back(std::move(head));
	lines_.push_back(std::move(entry));
	final_newline_ = true;
}

bool Document::remove(std::string_view section, std::string_view key) {
	bool any = false;
	for (size_t i = lines_.size(); i-- > 0;)
		if (lines_[i].kind == Kind::Entry && lines_[i].section == section &&
				lines_[i].key == key) {
			lines_.erase(lines_.begin() + static_cast<long>(i));
			any = true;
		}
	return any;
}

std::vector<std::string> Document::sections() const {
	std::vector<std::string> out;
	for (const Line &l : lines_) {
		bool counts = l.kind == Kind::Section || (l.kind == Kind::Entry && l.section.empty());
		if (counts && std::find(out.begin(), out.end(), l.section) == out.end())
			out.push_back(l.section);
	}
	return out;
}

std::vector<std::pair<std::string, std::string>> Document::entries(
		std::string_view section) const {
	std::vector<std::pair<std::string, std::string>> out;
	for (const Line &l : lines_) {
		if (l.kind != Kind::Entry || l.section != section)
			continue;
		auto it = std::find_if(out.begin(), out.end(),
				[&l](const auto &e) { return e.first == l.key; });
		if (it != out.end())
			it->second = l.value;
		else
			out.emplace_back(l.key, l.value);
	}
	return out;
}

std::string Document::text() const {
	std::string out;
	for (size_t i = 0; i < lines_.size(); ++i) {
		out += lines_[i].raw;
		if (i + 1 < lines_.size() || final_newline_)
			out += "\n";
	}
	return out;
}

}  // namespace wb::ini
