#include "wb_test.hpp"

#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "waybox/atomic_file.hpp"

namespace {

std::string dir() {
	static std::string d = [] {
		char t[] = "/tmp/wb-atomic-XXXXXX";
		return std::string(mkdtemp(t));
	}();
	return d;
}

std::string read(const std::string &p) {
	std::ifstream f(p);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

int entries(const std::string &d) {
	int n = 0;
	if (DIR *dp = opendir(d.c_str())) {
		while (struct dirent *e = readdir(dp))
			if (e->d_name[0] != '.')
				++n;
		closedir(dp);
	}
	return n;
}

}  // namespace

/* Written whole; a new file gets the usual mode, an old one keeps its own. */
WB_TEST(atomic_write_and_mode) {
	std::string d = dir() + "/mode";
	mkdir(d.c_str(), 0700);
	std::string p = d + "/a.conf";
	std::string err;
	WB_CHECK(wb::write_file_atomic(p, "one\n", &err));
	WB_CHECK(read(p) == "one\n");
	mode_t mask = umask(0);
	umask(mask);
	struct stat st;
	WB_CHECK(stat(p.c_str(), &st) == 0 && (st.st_mode & 0777) == (0666 & ~mask));
	chmod(p.c_str(), 0750);
	WB_CHECK(wb::write_file_atomic(p, "two\n", &err));
	WB_CHECK(read(p) == "two\n");
	WB_CHECK(stat(p.c_str(), &st) == 0 && (st.st_mode & 0777) == 0750);
	WB_CHECK(entries(d) == 1);  /* no temporary file left */
}

/* A symlink where the old fixed temporary name was (or anything else
 * beside the file) isn't written through. */
WB_TEST(atomic_write_ignores_a_planted_link) {
	std::string d = dir() + "/link";
	mkdir(d.c_str(), 0700);
	std::string victim = d + "/victim";
	{
		std::ofstream(victim) << "precious\n";
	}
	std::string p = d + "/settings.ini";
	symlink(victim.c_str(), (p + ".wbconf-new").c_str());
	std::string err;
	WB_CHECK(wb::write_file_atomic(p, "[a]\nb=1\n", &err));
	WB_CHECK(read(victim) == "precious\n");
	WB_CHECK(read(p) == "[a]\nb=1\n");
}

/* Failing (no such directory): false, said why, nothing left behind. */
WB_TEST(atomic_write_failure) {
	std::string err;
	WB_CHECK(!wb::write_file_atomic(dir() + "/missing/x.conf", "x", &err));
	WB_CHECK(err.find("missing/x.conf") != std::string::npos);
}
