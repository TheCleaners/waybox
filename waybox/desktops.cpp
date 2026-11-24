/*
 * Pure virtual-desktop logic. Standard-library only, so it links into the
 * standalone unit test.
 */
#include "waybox/desktops.hpp"

#include <charconv>

namespace wb {

std::optional<int> desktop_target(std::string_view to, int current, int last,
		int count, bool wrap) {
	if (count <= 0)
		return std::nullopt;
	if (to.empty() || to == "current")
		return current;
	if (to == "last")
		return (last >= 0 && last < count) ? last : current;
	if (to == "up" || to == "down")
		return current;

	int step = 0;
	if (to == "next" || to == "right")
		step = 1;
	else if (to == "previous" || to == "left")
		step = -1;
	if (step != 0) {
		int target = current + step;
		if (target < 0 || target >= count)
			target = wrap ? (target + count) % count : current;
		return target;
	}

	int n = 0;
	auto [end, ec] = std::from_chars(to.data(), to.data() + to.size(), n);
	if (ec != std::errc{} || end != to.data() + to.size() || n < 1 || n > count)
		return std::nullopt;
	return n - 1;
}

std::string desktop_name(const DesktopConfig &config, int index) {
	if (index >= 0 && index < static_cast<int>(config.names.size()) &&
			!config.names[index].empty())
		return config.names[index];
	return "Desktop " + std::to_string(index + 1);
}

int parse_desktop_count(std::string_view text, int fallback) {
	while (!text.empty() && (text.front() == ' ' || text.front() == '\n' ||
			text.front() == '\t'))
		text.remove_prefix(1);
	while (!text.empty() && (text.back() == ' ' || text.back() == '\n' ||
			text.back() == '\t'))
		text.remove_suffix(1);
	int n = 0;
	auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
	if (ec != std::errc{} || end != text.data() + text.size())
		return fallback;
	if (n < 1)
		return 1;
	return n > kMaxDesktops ? kMaxDesktops : n;
}

}  // namespace wb
