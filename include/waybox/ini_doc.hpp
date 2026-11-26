#ifndef WB_INI_DOC_HPP
#define WB_INI_DOC_HPP

/*
 * An INI file to edit without disturbing it (Lua pages' wb.ini): lines left
 * alone come back byte for byte, comments and blank lines included; a
 * changed key keeps its line and spacing; a new key goes at the end of its
 * section, a new section at the end of the file.
 *
 * Keys before any [section] are in the section "". A value is everything
 * after the first '=' (trimmed): no comments within a line, as in mako's,
 * foot's and fuzzel's files, where '#' starts colors. Sections may repeat
 * (mako's criteria): a key's value is its last one; it's changed where it
 * last appears. Pure; unit-tested.
 */
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wb::ini {

class Document {
public:
	static Document parse(std::string_view text);

	std::optional<std::string> get(std::string_view section, std::string_view key) const;
	void set(std::string_view section, std::string_view key, std::string_view value);
	/* False if it wasn't there. */
	bool remove(std::string_view section, std::string_view key);
	/* A whole [section] (each time it appears): its header, its lines and
	 * the comments just above it. Not "" (the keys before any section). */
	bool remove_section(std::string_view section);
	/* Moved to the end of the file, as they are (each time it appears, in
	 * order): sections put in an order by moving each, in that order. */
	bool move_section_to_end(std::string_view section);
	/* The sections in order, each once ("" first if it has keys). */
	std::vector<std::string> sections() const;
	/* A section's keys and (last) values, in order. */
	std::vector<std::pair<std::string, std::string>> entries(std::string_view section) const;
	std::string text() const;

private:
	enum class Kind { Blank, Comment, Section, Entry, Other };
	struct Line {
		Kind kind = Kind::Other;
		std::string raw;
		std::string section;   /* the section it's in (a header: its own name) */
		std::string key, value;
		size_t value_at = 0;   /* Entry: where the value starts in raw */
	};
	std::vector<Line> lines_;
	bool final_newline_ = true;

	int last_entry(std::string_view section, std::string_view key) const;
	/* Its lines: [from, to) for each time it appears, from the comments
	 * just above its header to the next one's. */
	std::vector<std::pair<size_t, size_t>> section_spans(std::string_view section) const;
	size_t insert_at(std::string_view section) const;
};

}  // namespace wb::ini

#endif
