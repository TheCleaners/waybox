/*
 * Fuzz smoke test for the autostart-script and .desktop parsers
 * (startup_sources, integrations): fixed seeds, so it's repeatable, run
 * under the sanitizers in debug builds. Inputs: shell-token soup (quotes,
 * here-docs, continuations, functions, operators), mutations of a realistic
 * script, and raw bytes. Every entry point must return, without crashing;
 * and switching an entry on or off must change only that line.
 */
#include "wb_test.hpp"

#include <random>
#include <string>

#include "waybox/integrations.hpp"
#include "waybox/startup_sources.hpp"

namespace {

const char *const kTokens[] = {
	"swaybg", "mako", "&", "&&", "||", ";", "|", "(", ")", "{", "}", "if", "then", "fi",
	"else", "case", "esac", "in", "for", "do", "done", "while", "until", "function",
	"f()", "f", "()", "'", "\"", "\\", "#", "$(", "`", "2>&1", ">/dev/null", "<<EOF",
	"EOF", "<<-'X'", "X", "\t", "command", "-v", "which", "pkill", "exec", "\n", "\n",
	"\n", " ", " ", "\r", "=", "$x", "\xff", "\xc3\xa9", "load() {", "load &", "&>",
	">&", "&&&", "||||", "<<<", "\\\n"};

const char *kSeed = R"SH(#!/bin/sh
command -v kanshi >/dev/null 2>&1 && kanshi &
mako &
wall() {
    while :; do
        swaybg -m fill -i "$(pick)" &
        sleep 300
    done
}
wall &
cat > /tmp/x <<EOF
waybar &
EOF
long --opt \
    wlsunset &
if true; then
    foot --server &
fi
)SH";

std::string token(std::mt19937 &rng) {
	return kTokens[rng() % (sizeof kTokens / sizeof *kTokens)];
}

std::string input(std::mt19937 &rng) {
	std::string s;
	switch (rng() % 3) {
	case 0: {
		int len = static_cast<int>(rng() % 60);
		for (int i = 0; i < len; ++i)
			s += token(rng) + (rng() % 2 ? " " : "");
		break;
	}
	case 1: {
		s = kSeed;
		int edits = 1 + static_cast<int>(rng() % 20);
		for (int i = 0; i < edits && !s.empty(); ++i) {
			size_t at = rng() % s.size();
			switch (rng() % 3) {
			case 0: s.erase(at, rng() % 8); break;
			case 1: s.insert(at, token(rng)); break;
			default: s[at] = static_cast<char>(rng()); break;
			}
		}
		break;
	}
	default: {
		int len = static_cast<int>(rng() % 400);
		for (int i = 0; i < len; ++i)
			s += static_cast<char>(rng());
	}
	}
	return s;
}

size_t count_lines(const std::string &s) {
	size_t n = 1;
	for (char c : s)
		n += c == '\n';
	return n;
}

}  // namespace

WB_TEST(parsers_survive_fuzzing) {
	std::mt19937 rng(20261125);
	for (int round = 0; round < 6000; ++round) {
		std::string s = input(rng);
		for (const wb::ScriptEntry &e : wb::script_entries(s)) {
			std::string switched = wb::script_set_enabled(s, e.line, !e.enabled);
			/* Only that line changes: never a line added or lost. */
			WB_CHECK(count_lines(switched) == count_lines(s));
			(void)wb::script_start_command(e.command);
		}
		(void)wb::script_set_enabled(s, static_cast<int>(rng() % 50) - 5, rng() % 2);
		for (const char *p : {"swaybg", "mako", "f", "load", "waybar", ""})
			(void)wb::script_starters(s, p);
		(void)wb::programs_in_script(s, {"swaybg", "mako", "f"});
		(void)wb::script_start_command(s);
		(void)wb::version_from_output(s);
		wb::AutostartEntry entry;
		(void)wb::parse_autostart(s, {"Waybox"}, entry);
		(void)wb::autostart_set_enabled(s, rng() % 2);
	}
}
