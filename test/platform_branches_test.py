#!/usr/bin/env python3
"""Unit test: code that depends on the system follows include/waybox/
platform.hpp. Every preprocessor chain whose conditions name a system
(__linux__, __FreeBSD__, __DragonFly__, ...) and has an #else must say
WB_PLATFORM_FALLBACK in it: an #else that quietly means Linux compiles on
other systems and finds nothing there.

Usage: platform_branches_test.py <source root>
"""
import os
import re
import sys

SYSTEMS = re.compile(r"__(linux|FreeBSD|DragonFly|OpenBSD|NetBSD|APPLE|sun|unix)__|_WIN32")
DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)")
DIRS = ("waybox", "wbconf", "agent", "include", "msg")
EXTS = (".c", ".cpp", ".h", ".hpp")
# Branches on systems that aren't a choice among supported ones (POSIX
# spellings of one field): named here, with why.
ALLOWED = {
    ("waybox/app_menu.cpp", "__APPLE__"): "st_mtimespec vs POSIX st_mtim, not a fallback",
}


def check(path, rel):
    problems = []
    stack = []  # [mentions, has_else, else_ok, line, names]
    with open(path, errors="replace") as f:
        lines = f.readlines()
    for n, line in enumerate(lines, 1):
        m = DIRECTIVE.match(line)
        if m:
            kind, rest = m.group(1), m.group(2)
            if kind in ("if", "ifdef", "ifndef"):
                names = set(x.group(0) for x in SYSTEMS.finditer(rest))
                stack.append([bool(names), False, False, n, names])
            elif kind == "elif" and stack:
                names = set(x.group(0) for x in SYSTEMS.finditer(rest))
                stack[-1][0] |= bool(names)
                stack[-1][4] |= names
            elif kind == "else" and stack:
                stack[-1][1] = True
            elif kind == "endif" and stack:
                mentions, has_else, ok, start, names = stack.pop()
                allowed = all((rel, nm) in ALLOWED for nm in names)
                if mentions and has_else and not ok and not allowed:
                    problems.append(f"{rel}:{start}: an #else among systems "
                                    f"({', '.join(sorted(names))}) without WB_PLATFORM_FALLBACK")
            continue
        if stack and stack[-1][1] and "WB_PLATFORM_FALLBACK" in line:
            stack[-1][2] = True
    return problems


def main():
    root = sys.argv[1]
    problems = []
    seen = 0
    for d in DIRS:
        for dirpath, _, files in os.walk(os.path.join(root, d)):
            for name in files:
                if name.endswith(EXTS):
                    path = os.path.join(dirpath, name)
                    rel = os.path.relpath(path, root)
                    if rel == "include/waybox/platform.hpp":
                        continue  # (its comment shows the pattern)
                    seen += 1
                    problems += check(path, rel)
    for p in problems:
        print("FAIL:", p, file=sys.stderr)
    if seen < 50:
        print(f"FAIL: only {seen} sources found under {root}", file=sys.stderr)
        return 1
    if problems:
        return 1
    print(f"ok ({seen} sources)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
