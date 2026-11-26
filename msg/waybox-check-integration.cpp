/*
 * waybox-check-integration [--strict] FILE.integration...
 *
 * Each file as wbconf reads it (waybox-integration(5)), its problems as
 * "FILE:LINE: error|warning: ..." (compilers' form, for CI). Exit 1 on an
 * error (or, with --strict, a warning), 2 on a file that can't be read.
 * Needs nothing but the C++ library: packagers and the community
 * repository's CI run it without GTK or a display.
 */
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "waybox/integrations.hpp"

namespace fs = std::filesystem;

int main(int argc, char **argv) {
	bool strict = false;
	std::vector<std::string> files;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--strict") == 0)
			strict = true;
		else
			files.push_back(argv[i]);
	}
	if (files.empty()) {
		fprintf(stderr, "usage: waybox-check-integration [--strict] FILE.integration...\n");
		return 2;
	}
	int status = 0;
	for (const std::string &file : files) {
		std::ifstream in(file);
		if (!in) {
			fprintf(stderr, "%s: can't be read\n", file.c_str());
			status = 2;
			continue;
		}
		std::stringstream text;
		text << in.rdbuf();
		fs::path p(file);
		if (p.extension() != ".integration")
			printf("%s: warning: not named ID.integration: wbconf reads only those\n",
					file.c_str());
		wb::IntegrationCheck c = wb::check_integration(p.stem().string(), text.str());
		int errors = 0, warnings = 0;
		for (const wb::IntegrationProblem &pr : c.problems) {
			if (pr.line > 0)
				printf("%s:%d: %s: %s\n", file.c_str(), pr.line,
						pr.error ? "error" : "warning", pr.message.c_str());
			else
				printf("%s: %s: %s\n", file.c_str(), pr.error ? "error" : "warning",
						pr.message.c_str());
			(pr.error ? errors : warnings)++;
		}
		if (errors > 0 || (strict && warnings > 0))
			status = std::max(status, 1);
		if (c.problems.empty())
			printf("%s: ok (%s, %zu option%s)\n", file.c_str(), c.integration->name.c_str(),
					c.integration->options.size(),
					c.integration->options.size() == 1 ? "" : "s");
	}
	return status;
}
