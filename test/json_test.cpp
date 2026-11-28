#include <clocale>
#include <cmath>
#include <limits>
#include <cstdio>
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

WB_TEST(json_output_is_always_utf8) {
	/* A Latin-1 X11 title, a truncated sequence, an overlong form, a
	 * surrogate: each bad byte becomes U+FFFD; good UTF-8 stays. */
	std::string out = wb::json::dump(Value(std::string("caf\xe9 \xe2\x82 \xc0\xaf \xed\xa0\x80 \xc3\xa9")));
	WB_CHECK(out == "\"caf\xef\xbf\xbd \xef\xbf\xbd\xef\xbf\xbd \xef\xbf\xbd\xef\xbf\xbd "
			"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd \xc3\xa9\"");
	WB_CHECK(wb::json::parse(out).has_value());
	WB_CHECK(!wb::json::parse(R"("\udc00")"));  /* a lone low surrogate */
}

/* Numbers as JSON has them: what strtod would take but JSON doesn't is
 * refused; good ones (fractions, exponents, negative zero) read. */
WB_TEST(json_number_grammar) {
	for (const char *bad : {"01", "-01", ".5", "1.", "-", "1e", "1e+", "+1", "0x10", "1.5.2",
			"--1", "1ee2", "00"}) {
		std::string doc = std::string("{\"n\":") + bad + "}";
		WB_CHECK(!wb::json::parse(doc));
	}
	auto num = [](const char *text) {
		auto v = wb::json::parse(std::string("{\"n\":") + text + "}");
		const double *d = v ? v->get("n")->number() : nullptr;
		return d != nullptr ? *d : -12345.0;
	};
	WB_CHECK(num("0") == 0 && num("-0") == 0 && num("10") == 10);
	WB_CHECK(num("1.5") == 1.5 && num("-0.25") == -0.25);
	WB_CHECK(num("1e3") == 1000 && num("2E-2") == 0.02 && num("1.5e+2") == 150);
	WB_CHECK(!wb::json::parse("{\"n\":1e999}"));  /* overflow: not finite, refused */
}

/* Under a locale that writes decimals with a comma (where one is
 * installed), JSON still reads and writes a point. */
WB_TEST(json_numbers_ignore_the_locale) {
	const char *got = nullptr;
	for (const char *l : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "nl_NL.UTF-8"})
		if ((got = setlocale(LC_NUMERIC, l)) != nullptr)
			break;
	if (got == nullptr) {
		fprintf(stderr, "json_numbers_ignore_the_locale: no comma locale here; skipped\n");
		return;
	}
	auto v = wb::json::parse("{\"n\":1.5}");
	WB_CHECK(v && v->get("n")->number() && *v->get("n")->number() == 1.5);
	WB_CHECK(wb::json::dump(wb::json::Value(2.25)) == "2.25");
	setlocale(LC_NUMERIC, "C");
}

/* Raw UTF-8 in strings: whole and valid, or not JSON (escapes were
 * checked already). */
WB_TEST(json_strings_need_valid_utf8) {
	WB_CHECK(wb::json::parse("{\"t\":\"caf\xc3\xa9 \xe2\x9c\x93\"}"));
	for (const char *bad : {"\xff", "\xc3", "\xc3(", "\xe2\x9c", "\xc0\xaf", "\xed\xa0\x80",
			"\xf4\x90\x80\x80"}) {
		std::string doc = std::string("{\"t\":\"a") + bad + "b\"}";
		WB_CHECK(!wb::json::parse(doc));
	}
	auto v = wb::json::parse("{\"t\":\"\xc3\xa9\"}");
	WB_CHECK(v && v->str("t") == "\xc3\xa9");
}

/* Written numbers read back as the same number; one JSON can't say is
 * written as null. */
WB_TEST(json_numbers_round_trip) {
	for (double d : {0.1, 1.0 / 3, std::nextafter(1.0, 2.0), 1e-300, -2.5e17, 123456.789}) {
		auto v = wb::json::parse("{\"n\":" + wb::json::dump(wb::json::Value(d)) + "}");
		WB_CHECK(v && v->get("n")->number() && *v->get("n")->number() == d);
	}
	WB_CHECK(wb::json::dump(wb::json::Value(0.1)) == "0.1");
	WB_CHECK(wb::json::dump(wb::json::Value(std::numeric_limits<double>::infinity())) == "null");
	WB_CHECK(wb::json::dump(wb::json::Value(std::nan(""))) == "null");
}

/* 64 containers deep at most, empty ones counted as any other. */
WB_TEST(json_depth_limit) {
	auto nested = [](int n, const char *open, const char *close) {
		std::string s;
		for (int k = 0; k < n; ++k)
			s += open;
		for (int k = 0; k < n; ++k)
			s += close;
		return s;
	};
	WB_CHECK(wb::json::parse(nested(64, "[", "]")));
	WB_CHECK(!wb::json::parse(nested(65, "[", "]")));
	WB_CHECK(!wb::json::parse(nested(66, "[", "]")));
	std::string objs;
	for (int k = 0; k < 64; ++k)
		objs += "{\"a\":";
	objs += "{}";
	for (int k = 0; k < 64; ++k)
		objs += "}";
	WB_CHECK(!wb::json::parse(objs));  /* 65 objects deep */
}
