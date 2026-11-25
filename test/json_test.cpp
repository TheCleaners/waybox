#include "wb_test.hpp"

#include "waybox/json.hpp"

using wb::json::Value;

WB_TEST(json_parses_requests) {
	auto v = wb::json::parse(R"( {"command":"action","action":"GoToDesktop","to":2,
		"window":"app_id:foot","flags":[true,false,null],"x":-1.5e2} )");
	WB_CHECK(v.has_value());
	WB_CHECK(v->str("command") == "action");
	WB_CHECK(v->str("to") == "2");               /* numbers read as strings */
	WB_CHECK(v->get("flags")->array()->size() == 3);
	WB_CHECK(*v->get("x")->number() == -150);
	WB_CHECK(v->str("missing", "dflt") == "dflt");
}

WB_TEST(json_strings_escape_both_ways) {
	auto v = wb::json::parse(R"("a\"b\\c\n\u00e9\ud83d\ude00")");
	WB_CHECK(v && *v->string() == "a\"b\\c\n\xc3\xa9\xf0\x9f\x98\x80");
	std::string out = wb::json::dump(Value(std::string("tab\there \"q\" \x01")));
	WB_CHECK(out == "\"tab\\there \\\"q\\\" \\u0001\"");
	WB_CHECK(wb::json::parse(out)->string() != nullptr);
}

WB_TEST(json_dumps_one_line) {
	wb::json::Object o;
	o["name"] = "foot";
	o["id"] = 3;
	o["focused"] = true;
	o["geometry"] = wb::json::Array{Value(1), Value(2.5)};
	std::string line = wb::json::dump(Value(o));
	WB_CHECK(line == R"({"focused":true,"geometry":[1,2.5],"id":3,"name":"foot"})");
	WB_CHECK(line.find('\n') == std::string::npos);
	WB_CHECK(*wb::json::parse(line) == Value(o));
}

WB_TEST(json_rejects_garbage) {
	for (const char *bad : {"", "{", "{\"a\":}", "[1,]", "\"unterminated", "tru",
			"{\"a\":1} x", "\"\\q\"", "1e999", "\"\x01\""})
		WB_CHECK(!wb::json::parse(bad));
	std::string deep(100, '[');
	WB_CHECK(!wb::json::parse(deep + std::string(100, ']')));
}
