#include "waybox/change_list.hpp"

#include <cstdint>
#include <vector>

namespace wb {

namespace {

const char *kReplacement = "\xEF\xBF\xBD";
const char *kEllipsis = "\xE2\x80\xA6";

/* The length of the valid UTF-8 sequence at s[i], or 0. */
size_t utf8_len(std::string_view s, size_t i) {
	auto b = [&](size_t k) { return static_cast<uint8_t>(s[k]); };
	uint8_t c = b(i);
	size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
	if (n == 0 || i + n > s.size())
		return 0;
	for (size_t k = 1; k < n; ++k)
		if ((b(i + k) & 0xC0) != 0x80)
			return 0;
	uint32_t cp = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
	for (size_t k = 1; k < n; ++k)
		cp = (cp << 6) | (b(i + k) & 0x3F);
	/* Overlong forms, surrogates, beyond U+10FFFF. */
	if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000) ||
			(cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
		return 0;
	return n;
}

}  // namespace

std::string one_line(std::string_view s, size_t max_chars) {
	/* Characters first (each a string), then cut. */
	std::vector<std::string> chars;
	bool space = false;
	for (size_t i = 0; i < s.size();) {
		unsigned char c = static_cast<unsigned char>(s[i]);
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
			space = !chars.empty();
			++i;
			continue;
		}
		size_t n = utf8_len(s, i);
		std::string ch;
		if (n == 0) {
			ch = kReplacement;
			n = 1;
		} else if (n == 1 && (c < 0x20 || c == 0x7F)) {
			i += 1;  /* other control characters: dropped */
			continue;
		} else {
			ch = std::string(s.substr(i, n));
		}
		if (space)
			chars.emplace_back(" ");
		space = false;
		chars.push_back(std::move(ch));
		i += n;
	}
	std::string out;
	if (chars.size() <= max_chars) {
		for (const std::string &ch : chars)
			out += ch;
		return out;
	}
	if (max_chars == 0)
		return out;
	size_t keep = max_chars - 1;
	while (keep > 0 && chars[keep - 1] == " ")
		--keep;
	for (size_t k = 0; k < keep; ++k)
		out += chars[k];
	return out + kEllipsis;
}

std::string change_values(const ChangeItem &c, size_t max_chars) {
	if (c.before.empty() && c.after.empty())
		return "";
	return one_line(c.before, max_chars) + " \xE2\x86\x92 " + one_line(c.after, max_chars);
}

std::string change_tooltip(const std::vector<ChangeItem> &changes, size_t max_lines,
		size_t max_chars, const std::string &and_more_format) {
	std::string out;
	size_t shown = 0;
	for (const ChangeItem &c : changes) {
		if (shown == max_lines)
			break;
		if (!out.empty())
			out += "\n";
		out += one_line(c.where, max_chars);
		std::string v = change_values(c, max_chars / 2 > 0 ? max_chars / 2 : 1);
		if (!v.empty())
			out += ": " + v;
		++shown;
	}
	if (changes.size() > shown) {
		std::string more = and_more_format;
		size_t at = more.find("%zu");
		if (at != std::string::npos)
			more.replace(at, 3, std::to_string(changes.size() - shown));
		out += "\n" + more;
	}
	return out;
}

}  // namespace wb
