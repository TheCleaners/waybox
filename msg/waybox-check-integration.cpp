/*
 * waybox-check-integration [--strict] [--runner PATH] FILE.integration...
 *
 * Each file as wbconf reads it (waybox-integration(5)), its problems as
 * "FILE:LINE: error|warning: ..." (compilers' form, for CI). Exit 1 on an
 * error (or, with --strict, a warning), 2 on a file that can't be read.
 * Needs nothing but the C++ library: packagers and the community
 * repository's CI run it without GTK or a display.
 *
 * Its Lua page (Page=NAME.lua), if wbconf-lua is there (--runner, or
 * $WBCONF_LUA, or where Waybox installs it): run as wbconf runs it, with an
 * empty home folder (as a new user's, its files not there yet), then
 * saved with its own values; nothing written. Its errors said with their
 * line, "NAME.lua:LINE: error: ...".
 */
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <optional>
#include <string>
#include <vector>

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "waybox/integrations.hpp"
#include "waybox/json.hpp"
#include "waybox/page_protocol.hpp"

namespace fs = std::filesystem;
namespace json = wb::json;

namespace {

struct PageRun {
	bool ran = false;          /* the runner started and answered */
	size_t controls = 0;       /* the page's, when it described one */
	std::vector<std::pair<int, std::string>> errors;  /* line (0: none), message */
};

/* A line from the runner, within `ms`; nullopt at its end or the time up. */
std::optional<std::string> read_line(int fd, std::string &buf, int ms) {
	for (;;) {
		size_t nl = buf.find('\n');
		if (nl != std::string::npos) {
			std::string line = buf.substr(0, nl);
			buf.erase(0, nl + 1);
			return line;
		}
		struct pollfd p = {fd, POLLIN, 0};
		if (poll(&p, 1, ms) <= 0)
			return std::nullopt;
		char chunk[4096];
		ssize_t n = read(fd, chunk, sizeof chunk);
		if (n <= 0)
			return std::nullopt;
		buf.append(chunk, static_cast<size_t>(n));
	}
}

/* One message, and its replies up to {"done": true}. */
std::optional<std::vector<json::Value>> ask(int to, int from, std::string &buf,
		const json::Value &msg) {
	std::string line = json::dump(msg) + "\n";
	if (write(to, line.data(), line.size()) != static_cast<ssize_t>(line.size()))
		return std::nullopt;
	std::vector<json::Value> out;
	for (;;) {
		auto l = read_line(from, buf, 5000);
		if (!l)
			return std::nullopt;
		auto v = json::parse(*l);
		if (!v)
			continue;
		if (const json::Value *d = v->get("done"); d && d->boolean() && *d->boolean())
			return out;
		out.push_back(*v);
	}
}

void take_errors(const std::vector<json::Value> &replies, PageRun &r) {
	for (const json::Value &m : replies)
		if (const json::Value *e = m.get("error")) {
			const json::Value *l = e->get("line");
			int line = l && l->number() ? static_cast<int>(*l->number()) : 0;
			r.errors.emplace_back(line, e->str("message"));
		}
}

/* The page, run by `runner` as wbconf runs it, in an empty home. */
PageRun run_page(const std::string &runner, const wb::Integration &in,
		const std::string &script) {
	PageRun r;
	char tmpl[] = "/tmp/waybox-check-XXXXXX";
	const char *home = mkdtemp(tmpl);
	if (home == nullptr)
		return r;
	int in_pipe[2], out_pipe[2];
	if (pipe(in_pipe) != 0)
		return r;
	if (pipe(out_pipe) != 0) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		return r;
	}
	pid_t pid = fork();
	if (pid == 0) {
		dup2(in_pipe[0], 0);
		dup2(out_pipe[1], 1);
		close(in_pipe[1]);
		close(out_pipe[0]);
		execl(runner.c_str(), runner.c_str(), static_cast<char *>(nullptr));
		_exit(127);
	}
	close(in_pipe[0]);
	close(out_pipe[1]);
	if (pid > 0) {
		signal(SIGPIPE, SIG_IGN);
		std::string buf;
		json::Array files, runs;
		for (const std::string &f : in.files)
			files.emplace_back(f);
		for (const std::string &x : in.runs)
			runs.emplace_back(x);
		auto opened = ask(in_pipe[1], out_pipe[0], buf, json::Object{{"open", json::Object{
				{"script", script}, {"files", files}, {"runs", runs}, {"home", home}}}});
		if (opened) {
			r.ran = true;
			take_errors(*opened, r);
			for (const json::Value &m : *opened)
				if (const json::Value *pv = m.get("page")) {
					if (auto page = wb::page::page_from_json(*pv)) {
						r.controls = page->controls.size();
						/* Saved with its own values. */
						json::Object vals;
						for (const auto &[k, v] : wb::page::page_values(*page))
							vals[k] = v;
						if (auto saved = ask(in_pipe[1], out_pipe[0], buf,
								json::Object{{"save", vals}}))
							take_errors(*saved, r);
						else
							r.errors.emplace_back(0, "saving it didn't finish");
					}
				}
		} else {
			r.errors.emplace_back(0, "the page didn't answer (ran too long, or stopped)");
		}
		close(in_pipe[1]);
		close(out_pipe[0]);
		kill(pid, SIGTERM);
		waitpid(pid, nullptr, 0);
	} else {
		close(in_pipe[1]);
		close(out_pipe[0]);
	}
	std::error_code ec;
	fs::remove_all(home, ec);
	return r;
}

std::string page_runner(const std::string &given) {
	if (!given.empty())
		return given;
	if (const char *e = getenv("WBCONF_LUA"); e && *e)
		return e;
	return WB_PAGE_RUNNER;
}

}  // namespace

int main(int argc, char **argv) {
	bool strict = false;
	std::string runner_arg;
	std::vector<std::string> files;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--strict") == 0)
			strict = true;
		else if (strcmp(argv[i], "--runner") == 0 && i + 1 < argc)
			runner_arg = argv[++i];
		else
			files.push_back(argv[i]);
	}
	std::string runner = page_runner(runner_arg);
	bool have_runner = access(runner.c_str(), X_OK) == 0;
	if (files.empty()) {
		fprintf(stderr, "usage: waybox-check-integration [--strict] [--runner PATH] "
				"FILE.integration...\n");
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
		/* Its Lua page, run. */
		if (!c.integration)
			continue;
		wb::Integration integ = *c.integration;
		integ.dir = p.parent_path().empty() ? "." : p.parent_path().string();
		std::string script = wb::lua_page_path(integ);
		if (script.empty())
			continue;
		if (!fs::exists(script)) {
			printf("%s: error: its page %s isn't there\n", file.c_str(), script.c_str());
			status = std::max(status, 1);
			continue;
		}
		if (!have_runner) {
			printf("%s: note: its page not run: no %s (Waybox built without Lua?)\n",
					script.c_str(), runner.c_str());
			continue;
		}
		PageRun r = run_page(runner, integ, fs::absolute(script).string());
		for (const auto &[line, msg] : r.errors) {
			if (line > 0)
				printf("%s:%d: error: %s\n", script.c_str(), line, msg.c_str());
			else
				printf("%s: error: %s\n", script.c_str(), msg.c_str());
		}
		if (!r.errors.empty() || !r.ran)
			status = std::max(status, 1);
		else
			printf("%s: ok (a page of %zu control%s, saved)\n", script.c_str(), r.controls,
					r.controls == 1 ? "" : "s");
	}
	return status;
}
