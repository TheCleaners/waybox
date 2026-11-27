#include "wb_test.hpp"

#include "waybox/change_list.hpp"

#include <algorithm>
#include <string>

namespace {

/* Characters (code points) in valid UTF-8. */
size_t chars(const std::string &s) {
	size_t n = 0;
	for (unsigned char c : s)
		n += (c & 0xC0) != 0x80;
	return n;
}

}  // namespace

WB_TEST(one_line_flattens) {
	WB_CHECK(wb::one_line("", 10).empty());
	WB_CHECK(wb::one_line("plain", 10) == "plain");
	WB_CHECK(wb::one_line("  a\n\n\tb  c \r\n", 10) == "a b c");
	/* Control characters other than white space go; DEL too. */
	WB_CHECK(wb::one_line(std::string("a\x01\x1b[31mb\x7f", 9), 20) == "a[31mb");
	/* Not UTF-8: U+FFFD per bad byte; overlong and surrogate forms too. */
	WB_CHECK(wb::one_line("a\xff" "b", 10) == "a\xEF\xBF\xBD" "b");
	WB_CHECK(wb::one_line("\xC0\xAF", 10) == "\xEF\xBF\xBD\xEF\xBF\xBD");
	WB_CHECK(wb::one_line("\xED\xA0\x80", 10) == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
	WB_CHECK(wb::one_line("\xE2\x82", 10) == "\xEF\xBF\xBD\xEF\xBF\xBD");  /* cut short */
	/* Valid multibyte kept whole. */
	WB_CHECK(wb::one_line("caf\xC3\xA9 \xF0\x9F\x99\x82", 10) == "caf\xC3\xA9 \xF0\x9F\x99\x82");
}

WB_TEST(one_line_cuts_by_character) {
	WB_CHECK(wb::one_line("abcdef", 6) == "abcdef");
	WB_CHECK(wb::one_line("abcdefg", 6) == "abcde\xE2\x80\xA6");
	/* Never inside a multibyte character, and counted as one. */
	std::string e(20, 'x');
	std::string accents;
	for (int i = 0; i < 20; ++i)
		accents += "\xC3\xA9";
	std::string cut = wb::one_line(accents, 5);
	WB_CHECK(cut == "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xE2\x80\xA6" && chars(cut) == 5);
	/* No space left before the "…". */
	WB_CHECK(wb::one_line("abc   defgh", 5) == "abc\xE2\x80\xA6");
	WB_CHECK(wb::one_line("abc", 0).empty());
	WB_CHECK(wb::one_line("abc", 1) == "\xE2\x80\xA6");
	/* A huge multi-line value stays within its length. */
	std::string big;
	for (int i = 0; i < 5000; ++i)
		big += "line " + std::to_string(i) + "\n";
	WB_CHECK(chars(wb::one_line(big, 60)) == 60);
}

WB_TEST(values_and_tooltip) {
	WB_CHECK(wb::change_values({"X", "", ""}, 10).empty());
	WB_CHECK(wb::change_values({"X", "", "on"}, 10) == " \xE2\x86\x92 on");
	WB_CHECK(wb::change_values({"X", "24", "32"}, 10) == "24 \xE2\x86\x92 32");
	std::vector<wb::ChangeItem> few = {{"Pointer size", "24", "32"}, {"Monitors", "", ""}};
	WB_CHECK(wb::change_tooltip(few, 8, 60, "and %zu more") ==
			"Pointer size: 24 \xE2\x86\x92 32\nMonitors");
	std::vector<wb::ChangeItem> many;
	for (int i = 0; i < 30; ++i)
		many.push_back({"Setting " + std::to_string(i), "a\nb", std::string(500, 'z')});
	std::string t = wb::change_tooltip(many, 8, 60, "and %zu more");
	size_t lines = 1;
	size_t longest = 0, cur = 0;
	for (size_t i = 0; i < t.size(); ++i) {
		if (t[i] == '\n') {
			++lines;
			longest = std::max(longest, cur);
			cur = 0;
		} else if ((static_cast<unsigned char>(t[i]) & 0xC0) != 0x80) {
			++cur;
		}
	}
	WB_CHECK(lines == 9);
	WB_CHECK(t.size() >= 11 && t.substr(t.size() - 11) == "and 22 more");
	WB_CHECK(longest <= 2 * 60 + 5);
	/* Exactly max_lines: no "more" line. */
	many.resize(8);
	WB_CHECK(wb::change_tooltip(many, 8, 60, "and %zu more").find("more") == std::string::npos);
}
