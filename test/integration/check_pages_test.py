#!/usr/bin/env python3
"""Integration test: waybox-check-integration runs an integration's Lua page.

Pages run through wbconf-lua as wbconf runs them, with an empty home folder,
then saved with their own values:
1. A page that copes with its file not there yet: ok, its controls counted.
2. A syntax error; a page counting on its file being there (nil from
   wb.read); an endless loop; an on_save that fails; a script that isn't
   there: each an error with its line (where there's one), exit 1.
3. The pages Waybox ships: all ok.
4. Without the runner: a note, not an error (a build without Lua).

Usage: check_pages_test.py <waybox-check-integration> <wbconf-lua> <integrations-dir>
"""
import os
import shutil
import subprocess
import sys
import tempfile

INTEGRATION = """[Integration]
Name={name}
Exec=true
Page={name}.lua
Files=~/.config/{name}/config
"""

PAGES = {
    "good": '''local text = wb.read("~/.config/good/config") or ""
page { switch { "on", label = "On", value = text:match("on") ~= nil },
       number { "n", label = "N", value = 3 } }
function on_save(v) wb.write("~/.config/good/config", v.on) end
''',
    "syntax": 'page { switch { "on", label = "On" }\n',
    "nofile": '''local text = wb.read("~/.config/nofile/config")
local n = text:match("n=(%d+)")
page { number { "n", label = "N", value = n } }
''',
    "loop": 'while true do end\n',
    "badsave": '''page { switch { "on", label = "On" } }
function on_save(v)
  error("cannot save")
end
''',
}


def run(checker, args, env):
    p = subprocess.run([checker] + args, capture_output=True, text=True, timeout=60, env=env)
    return p.returncode, p.stdout + p.stderr


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    checker, runner, shipped = sys.argv[1:]
    work = tempfile.mkdtemp(prefix="waybox-checkpages-", dir="/tmp")
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    env.pop("WBCONF_LUA", None)
    failures = []
    for name, code in list(PAGES.items()) + [("missing", None)]:
        with open(os.path.join(work, name + ".integration"), "w") as f:
            f.write(INTEGRATION.format(name=name))
        if code is not None:
            with open(os.path.join(work, name + ".lua"), "w") as f:
                f.write(code)

    def check(name):
        return run(checker, ["--runner", runner, os.path.join(work, name + ".integration")], env)

    # 1. A good page.
    rc, out = check("good")
    if rc != 0 or "good.lua: ok (a page of 2 controls, saved)" not in out:
        failures.append(f"a good page: {rc}\n{out}")
    # 2. Pages with mistakes.
    for name, want in (("syntax", "syntax.lua:"), ("nofile", "nofile.lua:2: error:"),
                       ("loop", "error:"), ("badsave", "badsave.lua:3: error:"),
                       ("missing", "isn't there")):
        rc, out = check(name)
        if rc != 1 or want not in out:
            failures.append(f"{name}: exit {rc}, not 1, or no {want!r}:\n{out}")
    rc, out = check("loop")
    if "longer" not in out and "didn't answer" not in out:
        failures.append(f"the endless loop not said so: {out}")
    # 3. The pages Waybox ships.
    files = sorted(os.path.join(shipped, f) for f in os.listdir(shipped)
                   if f.endswith(".integration"))
    rc, out = run(checker, ["--strict", "--runner", runner] + files, env)
    pages = [l for l in out.splitlines() if ".lua: ok (a page" in l]
    if rc != 0 or len(pages) < 3:
        failures.append(f"the shipped pages: exit {rc}, {len(pages)} pages ok:\n{out[-800:]}")
    # 4. No runner: a note.
    rc, out = run(checker, ["--runner", os.path.join(work, "nope"),
                            os.path.join(work, "good.integration")], env)
    if rc != 0 or "note: its page not run" not in out:
        failures.append(f"without a runner: {rc}\n{out}")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print("integration check pages: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
