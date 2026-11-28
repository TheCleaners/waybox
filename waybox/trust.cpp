#include "waybox/trust.hpp"

#include <climits>
#include <cstdlib>
#include <sys/stat.h>
#include <vector>

namespace wb {

namespace {

/* Every prefix of an absolute path, "/" first: "/a/b" -> "/", "/a", "/a/b". */
std::vector<std::string> prefixes(const std::string &path) {
	std::vector<std::string> out{"/"};
	std::string cur;
	size_t i = 0;
	while (i < path.size()) {
		while (i < path.size() && path[i] == '/')
			++i;
		size_t j = path.find('/', i);
		if (j == std::string::npos)
			j = path.size();
		if (j > i) {
			cur += "/" + path.substr(i, j - i);
			out.push_back(cur);
		}
		i = j;
	}
	return out;
}

/* One entry: `owner`'s, and (a directory or file, not a link) not open
 * to group or others' writing, unless a sticky directory. */
bool entry_ok(const struct stat &st, uid_t owner, bool last) {
	if (st.st_uid != owner && st.st_uid != 0)  /* (root's always is) */
		return false;
	if (S_ISLNK(st.st_mode))
		return true;  /* made by `owner` in a directory only it can write */
	bool others_write = (st.st_mode & (S_IWGRP | S_IWOTH)) != 0;
	if (!others_write)
		return true;
	/* Sticky: others may add to it, not replace this tree's next entry
	 * (checked in turn); a file itself must be closed. */
	return S_ISDIR(st.st_mode) && (st.st_mode & S_ISVTX) && !last;
}

bool chain_ok(const std::string &path, uid_t owner, bool follow) {
	std::vector<std::string> all = prefixes(path);
	for (size_t k = 0; k < all.size(); ++k) {
		struct stat st;
		if ((follow ? stat(all[k].c_str(), &st) : lstat(all[k].c_str(), &st)) != 0)
			return false;
		if (!entry_ok(st, owner, k + 1 == all.size()))
			return false;
	}
	return true;
}

}  // namespace

bool owned_by_system(const std::string &path, uid_t owner) {
	if (path.empty() || path[0] != '/')
		return false;
	char resolved[PATH_MAX];
	if (realpath(path.c_str(), resolved) == nullptr)
		return false;
	/* As spelled (each link's directory only `owner` could write), and as
	 * resolved (where the links lead). */
	return chain_ok(path, owner, false) && chain_ok(resolved, owner, true);
}

}  // namespace wb
