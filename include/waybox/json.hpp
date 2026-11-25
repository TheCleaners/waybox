#ifndef WB_JSON_HPP
#define WB_JSON_HPP

/*
 * A small JSON value with a parser and a writer, for the IPC socket
 * (newline-delimited JSON). Numbers are doubles; objects keep their keys
 * sorted. Pure; unit-tested.
 */
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wb::json {

struct Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

struct Value {
	std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v = nullptr;

	Value() = default;
	Value(std::nullptr_t) {}
	Value(bool b) : v(b) {}
	Value(int i) : v(static_cast<double>(i)) {}
	Value(double d) : v(d) {}
	Value(const char *s) : v(std::string(s)) {}
	Value(std::string s) : v(std::move(s)) {}
	Value(Array a) : v(std::move(a)) {}
	Value(Object o) : v(std::move(o)) {}

	bool is_null() const { return std::holds_alternative<std::nullptr_t>(v); }
	const std::string *string() const { return std::get_if<std::string>(&v); }
	const double *number() const { return std::get_if<double>(&v); }
	const bool *boolean() const { return std::get_if<bool>(&v); }
	const Array *array() const { return std::get_if<Array>(&v); }
	const Object *object() const { return std::get_if<Object>(&v); }
	/* A member of an object, or nullptr. */
	const Value *get(const std::string &key) const;
	/* A member as a string, or `fallback`. */
	std::string str(const std::string &key, const std::string &fallback = "") const;
	bool operator==(const Value &) const = default;
};

/* Parse one JSON text (nothing but whitespace after it); nullopt if invalid
 * or nested deeper than 64. */
std::optional<Value> parse(std::string_view text);
/* One line of JSON (no newlines in it). */
std::string dump(const Value &value);

}  // namespace wb::json

#endif
