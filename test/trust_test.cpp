#include "wb_test.hpp"

#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "waybox/trust.hpp"

namespace {

std::string dir() {
	static std::string d = [] {
		char t[] = "/tmp/wb-trust-XXXXXX";
		return std::string(mkdtemp(t));
	}();
	return d;
}

std::string file(const std::string &name, mode_t mode) {
	std::string p = dir() + "/" + name;
	std::ofstream(p) << "x\n";
	chmod(p.c_str(), mode);
	return p;
}

}  // namespace

/* The system's own: root's all the way up. */
WB_TEST(trust_system_files) {
	struct stat st;
	if (stat("/usr", &st) == 0 && st.st_uid == 0 && !(st.st_mode & 022))
		WB_CHECK(wb::owned_by_system("/usr"));
	WB_CHECK(!wb::owned_by_system("relative/path"));
	WB_CHECK(!wb::owned_by_system(""));
	WB_CHECK(!wb::owned_by_system("/no/such/place/at/all"));
}

/* Another owner's (as the test stands one in for root): theirs all the
 * way (a sticky /tmp above allowed), and no one else may write. */
WB_TEST(trust_owner_and_modes) {
	uid_t me = getuid();
	chmod(dir().c_str(), 0755);
	std::string ok = file("ok", 0644);
	WB_CHECK(wb::owned_by_system(ok, me));
	if (me != 0)
		WB_CHECK(!wb::owned_by_system(ok, 0));  /* not root's */
	WB_CHECK(!wb::owned_by_system(file("open", 0666), me));  /* others may write */
	WB_CHECK(!wb::owned_by_system(file("grp", 0664), me));   /* the group may */
	/* Its directory writable by others, not sticky: anyone could replace it. */
	std::string loose = dir() + "/loose";
	mkdir(loose.c_str(), 0777);
	chmod(loose.c_str(), 0777);
	std::string in_loose = loose + "/f";
	std::ofstream(in_loose) << "x\n";
	chmod(in_loose.c_str(), 0644);
	WB_CHECK(!wb::owned_by_system(in_loose, me));
	/* A link to it from a sound directory: followed, and the target fails. */
	std::string link = dir() + "/link";
	symlink(in_loose.c_str(), link.c_str());
	WB_CHECK(!wb::owned_by_system(link, me));
	/* A link to a sound file: fine. */
	std::string good_link = dir() + "/good-link";
	symlink(ok.c_str(), good_link.c_str());
	WB_CHECK(wb::owned_by_system(good_link, me));
}
