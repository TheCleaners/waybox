#ifndef WB_CHANGE_LIST_HPP
#define WB_CHANGE_LIST_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace wb {

/*
 * wbconf's list of changes not saved yet: each change a line of text,
 * kept short whatever the values hold (a multi-line command, a long font
 * description, bytes that aren't UTF-8), so a long list or an odd value
 * can't stretch a tooltip or popover across the screen.
 */

/* `s` on one line: runs of white space (line breaks, tabs) one space,
 * trimmed; other control characters dropped; bytes that aren't UTF-8
 * U+FFFD. At most `max_chars` characters: longer is cut, ending in "…". */
std::string one_line(std::string_view s, size_t max_chars);

struct ChangeItem {
	std::string where;   /* "Windows › Edge Tiling › Snap distance" */
	std::string before;  /* empty with `after`: just "changed" */
	std::string after;
};

/* "before → after", each side through one_line(`max_chars`); "" if both
 * are empty. */
std::string change_values(const ChangeItem &c, size_t max_chars);

/* A tooltip: up to `max_lines` lines "where: before → after", each at most
 * about 2×`max_chars` long, then "and N more" if there are. */
std::string change_tooltip(const std::vector<ChangeItem> &changes, size_t max_lines,
		size_t max_chars, const std::string &and_more_format);

}  // namespace wb

#endif
