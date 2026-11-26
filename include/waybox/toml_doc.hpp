#ifndef WB_TOML_DOC_HPP
#define WB_TOML_DOC_HPP

/*
 * A TOML file to edit without disturbing it (Lua pages' wb.toml), as
 * Alacritty's and wpaperd's: what isn't changed comes back byte for byte,
 * comments included; a changed value is replaced where it is (a float
 * stays a float); a new key goes after the last one of its table, in its
 * indentation, a new table at the end of the file under its own [header];
 * a removed key takes its line with it.
 *
 * A value is reached by its full path, however the file spells it:
 * [font] size = 11, font.size = 11 and font = { size = 11 } are all
 * { "font", "size" }; an array of tables' element by its (0-based) index:
 * { "bind", 0, "key" } for [[bind]]. Values come back as wb::jsonc::Value
 * (a date or time as its text). Pure; unit-tested.
 */
#include "waybox/jsonc_doc.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wb::toml {

using jsonc::Path;
using jsonc::Value;

class Document {
public:
	/* Text that isn't TOML: error() says where. */
	static Document parse(std::string_view text);
	bool ok() const { return error_.empty(); }
	const std::string &error() const { return error_; }

	std::optional<Value> get(const Path &path) const;
	/* Tables on the way that aren't there are made. False (nothing
	 * changed) through something that isn't a table, for a value TOML
	 * can't hold (null), or a new element of an array of tables. */
	bool set(const Path &path, const Value &value);
	/* A key (not a whole table); false if it wasn't there. */
	bool remove(const Path &path);
	const std::string &text() const { return text_; }

	/* Where things are, from parsing. */
	struct Entry {
		Path path;                  /* full */
		size_t line_start = 0;      /* its line (a key in a table) */
		size_t key_start = 0;
		size_t value_start = 0, value_end = 0;
		size_t line_end = 0;        /* past its comment and newline */
		bool inline_member = false; /* inside an inline table */
		Value::Type type = Value::Type::Null;
		bool is_float = false;
		bool is_inline_table = false;
		size_t section = 0;         /* its [table]'s index in sections */
	};
	struct Section {
		Path path;                  /* {} for the top of the file */
		size_t header_start = 0, header_end = 0;  /* the [header] line */
		bool array = false;         /* [[header]] */
	};

private:
	std::string text_;
	std::string error_;
	std::vector<Entry> entries_;
	std::vector<Section> sections_;
	Value tree_;

	void reparse();
};

/* As a new value is written: strings quoted, arrays and tables inline. */
std::string serialize(const Value &v);
/* A key as written: bare if it can be, else quoted. */
std::string key_text(const std::string &key);

}  // namespace wb::toml

#endif
