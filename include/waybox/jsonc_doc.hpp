#ifndef WB_JSONC_DOC_HPP
#define WB_JSONC_DOC_HPP

/*
 * A JSON file to edit without disturbing it (Lua pages' wb.json): JSON with
 * comments (// and slash-star) and trailing commas, as Waybar's config has
 * it. What isn't changed comes back byte for byte, comments included; a
 * changed value is replaced where it is; a new key goes after the last one
 * of its object, in that object's indentation and style (one member a
 * line, or all on one); a removed member takes its line with it.
 *
 * Values are reached by a path of object keys and (0-based) array indices.
 * Pure; unit-tested.
 */
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace wb::jsonc {

struct Value {
	enum class Type { Null, Bool, Integer, Number, String, Array, Object };
	Type type = Type::Null;
	bool boolean = false;
	int64_t integer = 0;
	double number = 0;
	std::string string;
	std::vector<Value> items;  /* Array */
	struct Member;
	std::vector<Member> members;  /* Object, in order */
};

struct Value::Member {
	Member(std::string k, Value v) : first(std::move(k)), second(std::move(v)) {}
	std::string first;  /* the key */
	Value second;
};

/* Parsed, with where each value is in the text. */
struct Node {
	Value::Type type = Value::Type::Null;
	size_t start = 0, end = 0;  /* the value's span */
	std::string scalar;         /* its text, for scalars */
	std::vector<Node> items;
	struct Member;
	std::vector<Member> members;
};

struct Node::Member {
	std::string key;
	size_t key_start = 0;
	Node node;
};

using PathPart = std::variant<std::string, size_t>;
using Path = std::vector<PathPart>;

class Document {
public:
	/* An empty (or blank) text is a document with nothing in it yet; set()
	 * starts an object. Text that isn't JSON(C): error() says where. */
	static Document parse(std::string_view text);
	bool ok() const { return error_.empty(); }
	const std::string &error() const { return error_; }

	std::optional<Value> get(const Path &path) const;
	/* Objects on the way that aren't there are made; an index may be one
	 * past the end (appending). False (nothing changed) through something
	 * that isn't an object or array, or past the end. */
	bool set(const Path &path, const Value &value);
	/* False if it wasn't there. */
	bool remove(const Path &path);
	const std::string &text() const { return text_; }

	using Node = jsonc::Node;

private:
	std::string text_;
	std::string error_;
	std::optional<Node> root_;

	void reparse();
	const Node *find(const Path &path) const;
	void edit(size_t at, size_t len, const std::string &with);
	std::string indent_unit() const;
};

/* Serialized as a new value would be, at `indent` (the line's), with
 * `unit` per level: scalars and arrays of scalars on one line, objects
 * and other arrays a member or item a line. */
std::string serialize(const Value &v, const std::string &indent = "",
		const std::string &unit = "    ");

}  // namespace wb::jsonc

#endif
