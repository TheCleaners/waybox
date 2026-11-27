#include "waybox/name_list.hpp"

#include <algorithm>

namespace wb::names {

namespace {

std::string_view trim(std::string_view s) {
	size_t b = s.find_first_not_of(" \t\r");
	if (b == std::string_view::npos)
		return {};
	size_t e = s.find_last_not_of(" \t\r");
	return s.substr(b, e - b + 1);
}

}  // namespace

List from_text(std::string_view text) {
	List out;
	size_t start = 0;
	while (start <= text.size()) {
		size_t nl = text.find('\n', start);
		std::string_view line = trim(text.substr(start, nl == std::string_view::npos
				? std::string_view::npos : nl - start));
		if (!line.empty())
			out.emplace_back(line);
		if (nl == std::string_view::npos)
			break;
		start = nl + 1;
	}
	return out;
}

std::string to_text(const List &list) {
	std::string out;
	for (size_t i = 0; i < list.size(); ++i)
		out += (i ? "\n" : "") + list[i];
	return out;
}

bool add(List &list, std::string_view name) {
	std::string_view t = trim(name);
	if (t.empty() || t.find('\n') != std::string_view::npos ||
			std::find(list.begin(), list.end(), t) != list.end())
		return false;
	list.emplace_back(t);
	return true;
}

bool move(List &list, size_t at, int by) {
	if (at >= list.size())
		return false;
	long to = static_cast<long>(at) + by;
	if (by == 0 || to < 0 || to >= static_cast<long>(list.size()))
		return false;
	std::string item = std::move(list[at]);
	list.erase(list.begin() + static_cast<long>(at));
	list.insert(list.begin() + to, std::move(item));
	return true;
}

bool remove(List &list, size_t at) {
	if (at >= list.size())
		return false;
	list.erase(list.begin() + static_cast<long>(at));
	return true;
}

List not_in(const List &suggestions, const List &list) {
	List out;
	for (const std::string &s : suggestions)
		if (std::find(list.begin(), list.end(), s) == list.end() &&
				std::find(out.begin(), out.end(), s) == out.end())
			out.push_back(s);
	return out;
}

}  // namespace wb::names
