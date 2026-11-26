#!/usr/bin/env python3
"""Test: the Lua Language Server hints (data/lua/waybox-page.lua) describe
everything wbconf-lua gives a page, so they don't fall behind it.

What the runner defines is read from its source (wbconf/page_runner.cpp):
the wb.* functions in C, the prelude's functions, setting kinds and
document methods. Each must appear in the hints file.

Usage: lua_stubs_test.py <page_runner.cpp> <waybox-page.lua>
"""
import re
import sys


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    src = open(sys.argv[1]).read()
    stub = open(sys.argv[2]).read()
    prelude = src[src.index("const char *kPrelude"):]
    prelude = prelude[:prelude.index(')LUA";')]
    want = set()
    # wb.* from C: {"read", wb_read}, ...
    table = src[src.index('{"read", wb_read}'):]
    table = table[:table.index("nullptr")]
    want |= {f"function wb.{n}(" for n in re.findall(r'\{"([a-z_]+)", wb_', table)}
    # The prelude's own: wb.x = function, function wb.a.b, function name(
    want |= {f"function wb.{n}(" for n in re.findall(r"^wb\.([a-z_]+) = function", prelude,
                                                     re.M)}
    want |= {f"function wb.{n}(" for n in re.findall(r"^function wb\.([a-z_.]+)\(", prelude,
                                                     re.M)}
    want |= {f"function {n}(" for n in re.findall(r"^function ([a-z_]+)\(", prelude, re.M)}
    # Setting kinds: for _, kind in ipairs { "text", ... } do
    kinds = re.search(r"for _, kind in ipairs \{([^}]*)\} do", prelude)
    if kinds is None:
        print("FAIL: the setting kinds not found in the runner", file=sys.stderr)
        return 1
    want |= {f"function {k}(" for k in re.findall(r'"([a-z]+)"', kinds.group(1))}
    # Documents' methods, by the class the hints give them.
    for cls, hint in (("Section", "Section"), ("Ini", "Ini"), ("Doc", "Doc")):
        want |= {f"function {hint}:{m}(" for m in
                 re.findall(rf"function {cls}:([a-z_]+)\(", prelude)}
    missing = sorted(w for w in want if w not in stub)
    if not want or missing:
        print(f"FAIL: not in the hints: {missing}", file=sys.stderr)
        return 1
    print(f"lua stubs: OK ({len(want)} names)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
