#include "wb_test.hpp"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <vector>

#include "../agent/audit.hpp"
#include "../agent/tunables.hpp"

using namespace wb;
using wb::agent::Audit;
using wb::agent::Tunables;

namespace {

struct Dir {
	std::string path;
	Dir() {
		char tmpl[] = "/tmp/wb-audit-XXXXXX";
		path = mkdtemp(tmpl);
	}
	~Dir() {
		std::string cmd = "rm -rf '" + path + "'";
		if (system(cmd.c_str()) != 0) {}
	}
};

std::vector<json::Value> lines(const std::string &file) {
	std::vector<json::Value> out;
	std::ifstream f(file);
	for (std::string l; std::getline(f, l);)
		if (auto v = json::parse(l))
			out.push_back(*v);
	return out;
}

mode_t mode_of(const std::string &p) {
	struct stat st;
	stat(p.c_str(), &st);
	return st.st_mode & 0777;
}

}  // namespace

WB_TEST(audit_lines_and_modes) {
	Dir d;
	std::string dir = d.path + "/state/agents/tools";
	Audit a(dir, Tunables{});
	WB_CHECK(a.ok());
	a.write(json::Object{{"event", "hub"}});
	a.write(json::Object{{"event", "call"}, {"call", "c-1"}}, true);
	auto got = lines(a.path());
	WB_CHECK(got.size() == 2 && got[0].str("event") == "hub" && got[1].str("call") == "c-1");
	WB_CHECK(got[0].get("v") != nullptr && got[0].str("time").size() == 24 &&
			got[0].str("time").back() == 'Z');
	WB_CHECK(mode_of(a.path()) == 0600 && mode_of(dir) == 0700);
}

WB_TEST(audit_appends_never_rewrites) {
	Dir d;
	{
		Audit a(d.path, Tunables{});
		a.write(json::Object{{"event", "first"}});
	}
	Audit again(d.path, Tunables{});
	again.write(json::Object{{"event", "second"}});
	auto got = lines(again.path());
	WB_CHECK(got.size() == 2 && got[0].str("event") == "first");
}

WB_TEST(audit_rotates) {
	Dir d;
	Tunables t;
	t.audit_rotate = 4096;
	t.audit_keep = 2;
	Audit a(d.path, t);
	for (int i = 0; i < 200; ++i)
		a.write(json::Object{{"event", "call"}, {"n", static_cast<double>(i)},
				{"pad", std::string(100, 'x')}});
	struct stat st;
	WB_CHECK(stat((d.path + "/audit.log.1").c_str(), &st) == 0);
	WB_CHECK(stat((d.path + "/audit.log.2").c_str(), &st) == 0);
	WB_CHECK(stat((d.path + "/audit.log.3").c_str(), &st) != 0);  /* (2 kept) */
	WB_CHECK(stat(a.path().c_str(), &st) == 0 && st.st_size <= 4096);
	/* The newest is in the current file. */
	auto got = lines(a.path());
	WB_CHECK(!got.empty() && *got.back().get("n")->number() == 199);
}

WB_TEST(audit_recovers_interrupted_intents) {
	Dir d;
	{
		Audit a(d.path, Tunables{});
		a.write(json::Object{{"event", "intent"}, {"call", "c-1"}, {"tool", "close_window"}}, true);
		a.write(json::Object{{"event", "intent"}, {"call", "c-2"}, {"tool", "run_command"}}, true);
		a.write(json::Object{{"event", "call"}, {"call", "c-2"}, {"outcome", "completed"}});
		/* (the hub crashes here: c-1 was dispatched, never answered) */
	}
	Audit after(d.path, Tunables{});
	WB_CHECK(after.recover() == 1);
	auto got = lines(after.path());
	const json::Value &last = got.back();
	WB_CHECK(last.str("call") == "c-1" && last.str("event") == "call" &&
			last.str("outcome") == "outcome-unknown" && last.str("tool") == "close_window");
	/* Recovered once only. */
	Audit again(d.path, Tunables{});
	WB_CHECK(again.recover() == 0);
}

WB_TEST(audit_args_redacted_and_cut) {
	Dir d;
	Tunables t;
	t.audit_cut = 10;
	Audit a(d.path, t);
	json::Value args = json::Object{
		{"password", "hunter2"},
		{"text", "0123456789abcdef"},
		{"nested", json::Object{{"password", "x"}, {"ok", "fine"}}},
		{"list", json::Array{"short", "0123456789xyz"}},
		{"n", 5.0},
	};
	json::Value got = a.audit_args(args, {"password"});
	WB_CHECK(got.str("password") == "<redacted>");
	WB_CHECK(got.get("nested")->str("password") == "<redacted>" && got.get("nested")->str("ok") == "fine");
	const json::Value *cut = got.get("text");
	WB_CHECK(cut->get("cut") != nullptr && *cut->get("length")->number() == 16 &&
			cut->str("start") == "0123456789");
	WB_CHECK(got.get("list")->array()->at(0).string() && *got.get("list")->array()->at(1).get("length")->number() == 13);
	WB_CHECK(*got.get("n")->number() == 5);
	/* Never cut inside a character. */
	json::Value utf = a.audit_args(json::Value("ééééééééé"), {});
	WB_CHECK(utf.str("start") == "ééééé");
}

WB_TEST(audit_refuses_a_link) {
	Dir d;
	std::string target = d.path + "/elsewhere";
	{
		std::ofstream(target) << "";
	}
	if (symlink(target.c_str(), (d.path + "/audit.log").c_str()) != 0)
		return;
	Audit a(d.path, Tunables{});
	WB_CHECK(!a.ok());
}
