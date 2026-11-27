#!/usr/bin/env python3
"""Integration test: wbconf-lua, the runner of integrations' Lua pages.

Driven over its protocol (JSON lines on stdin and stdout), as wbconf does:
1. A page read from a script and a config file it may read (Files=): its
   controls, values from the file.
2. A change: the page's on_change answers with an update (a problem shown).
3. A button: its request to run a program (Runs=); one not in Runs= is an
   error, said with its line.
4. Save: on_save's write (the new file's text) and run, as requests.
5. The sandbox: no io, os or require; reading or writing a file not in
   Files= is an error; an endless loop is stopped within its time budget,
   and the page still answers after it.
6. Mistakes said with their line: an undeclared global, a setting's name
   misspelled (with the right one suggested), a page without page { }.
7. Compiled Lua (bytecode) instead of source: refused, not run.

Usage: page_runner_test.py <waybox> <wbconf-lua>   (waybox: unused, as the
harness passes it first)
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

PAGE = r'''
local text = wb.read("~/.config/demo/config") or ""
local timeout = text:match("timeout=(%d+)") or "5000"

page {
  group "Where" {
    choice { "anchor", label = "Corner", value = "top-right",
             choices = { "top-left", "top-right" } },
  },
  number { "timeout", label = "Disappear after", unit = "ms", min = 0, max = 60000,
           value = timeout },
  switch { "icons", label = "Icons", value = true },
  button { "Send a test", key = "test", on_click = function()
    wb.run { "notify-send", "Hello" }
  end },
  button { "Bad", key = "bad", on_click = function()
    wb.run { "rm", "-rf", "/" }
  end },
  button { "Spin", key = "spin", on_click = function()
    while true do end
  end },
  button { "Escape", key = "escape", on_click = function()
    return io.open("/etc/passwd")
  end },
  button { "Peek", key = "peek", on_click = function()
    wb.read("/etc/passwd")
  end },
  button { "Typo", key = "typo", on_click = function()
    wb.set("timout", { value = 1 })
  end },
  button { "Global", key = "global", on_click = function()
    counter = 1
  end },
}

function on_change(key, value)
  if key == "timeout" and tonumber(value) < 1000 then
    wb.problem("timeout", "Under a second: hard to read")
  else
    wb.problem("timeout", "")
  end
end

function on_save(values)
  wb.write("~/.config/demo/config", "timeout=" .. values.timeout .. "\n")
  wb.run { "makoctl", "reload" }
end
'''


class Runner:
    def __init__(self, exe, env):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, env=env)

    def ask(self, msg, timeout=5.0):
        """Send one message; the replies up to {"done": true}."""
        self.p.stdin.write(json.dumps(msg) + "\n")
        self.p.stdin.flush()
        out = []
        end = time.time() + timeout
        while time.time() < end:
            line = self.p.stdout.readline()
            if not line:
                break
            m = json.loads(line)
            if m.get("done"):
                return out
            out.append(m)
        out.append({"timeout": True})
        return out

    def close(self):
        if self.p.poll() is None:
            self.p.stdin.close()
            try:
                self.p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.p.kill()


def errors(replies):
    return [r["error"] for r in replies if "error" in r]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    exe = sys.argv[2]
    work = tempfile.mkdtemp(prefix="waybox-pagerun-", dir="/tmp")
    home = os.path.join(work, "home")
    os.makedirs(os.path.join(home, ".config", "demo"))
    with open(os.path.join(home, ".config", "demo", "config"), "w") as f:
        f.write("timeout=7000\n")
    script = os.path.join(work, "demo.lua")
    with open(script, "w") as f:
        f.write(PAGE)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    failures = []
    opened = {"open": {"script": script, "home": home,
                       "files": ["~/.config/demo/config"], "runs": ["notify-send", "makoctl"]}}
    r = Runner(exe, env)
    try:
        # 1. The page.
        replies = r.ask(opened)
        pages = [m["page"] for m in replies if "page" in m]
        if len(pages) != 1 or errors(replies):
            failures.append(f"open: {replies}")
        else:
            cs = pages[0]["controls"]
            kinds = [c["kind"] for c in cs]
            if kinds[:3] != ["group", "number", "switch"] or cs[1]["value"] != "7000" or \
                    cs[2]["value"] != "on" or cs[0]["children"][0]["key"] != "anchor":
                failures.append(f"the page as described: {cs[:3]}")
        # 2. A change: a problem.
        replies = r.ask({"change": {"key": "timeout", "value": "500"}})
        if replies != [{"update": {"timeout": {"problem": "Under a second: hard to read"}}}]:
            failures.append(f"change: {replies}")
        # 3. A button's run; one not allowed.
        replies = r.ask({"click": "test"})
        if replies != [{"run": ["notify-send", "Hello"]}]:
            failures.append(f"click test: {replies}")
        replies = r.ask({"click": "bad"})
        e = errors(replies)
        if not e or "isn't in the integration's Runs=" not in e[0]["message"] or \
                e[0]["line"] != 17 or any("run" in m for m in replies):
            failures.append(f"a run not in Runs=: {replies}")
        # 4. Save.
        replies = r.ask({"save": {"timeout": "3000", "anchor": "top-left", "icons": ""}})
        want_path = os.path.join(home, ".config", "demo", "config")
        if replies != [{"write": {"path": want_path, "text": "timeout=3000\n"}},
                       {"run": ["makoctl", "reload"]}]:
            failures.append(f"save: {replies}")
        # 5. The sandbox.
        t0 = time.time()
        replies = r.ask({"click": "spin"})
        spent = time.time() - t0
        e = errors(replies)
        if not e or "took longer" not in e[0]["message"] or spent > 2.0:
            failures.append(f"an endless loop: {replies} after {spent:.1f}s")
        replies = r.ask({"click": "escape"})
        e = errors(replies)
        if not e or "'io' isn't defined" not in e[0]["message"]:
            failures.append(f"io: {replies}")
        replies = r.ask({"click": "peek"})
        e = errors(replies)
        if not e or "isn't in the integration's Files=" not in e[0]["message"]:
            failures.append(f"reading a file not in Files=: {replies}")
        # 6. Mistakes, with their line.
        replies = r.ask({"click": "typo"})
        e = errors(replies)
        if not e or e[0]["message"] != "'timout' is not a setting of this page (timeout?)" or \
                e[0]["line"] != 29:
            failures.append(f"a misspelled setting: {replies}")
        replies = r.ask({"click": "global"})
        e = errors(replies)
        if not e or "assigning the global 'counter'" not in e[0]["message"] or e[0]["line"] != 32:
            failures.append(f"an undeclared global: {replies}")
        # Still answering after all that.
        replies = r.ask({"change": {"key": "timeout", "value": "5000"}})
        if replies != [{"update": {"timeout": {"problem": ""}}}]:
            failures.append(f"after the errors: {replies}")
    finally:
        r.close()

    # A script with no page, and one that doesn't load.
    # Compiled Lua (bytecode) isn't a page: only source is loaded. The page
    # itself compiled where luajit is there (it would run as it is), else a
    # bytecode header.
    bytecode = b"\x1bLJ\x02\x00junk"
    if shutil.which("luajit"):
        src = os.path.join(work, "compiled-src.lua")
        out = os.path.join(work, "compiled.out")
        with open(src, "w") as f:
            f.write(PAGE)
        if subprocess.run(["luajit", "-b", src, out], capture_output=True).returncode == 0:
            bytecode = open(out, "rb").read()
    with open(script, "wb") as f:
        f.write(bytecode)
    r = Runner(exe, env)
    try:
        replies = r.ask(opened)
        e = errors(replies)
        if not e or "source" not in e[0]["message"] or any("page" in m for m in replies):
            failures.append(f"compiled Lua was loaded: {replies}")
    finally:
        r.close()

    for code, want in (("local x = 1\n", "page { ... } is missing"),
                       ("page {\n  number { }\n}\n", "needs its key first"),
                       ("page { label 'x' \n", "expected")):
        with open(script, "w") as f:
            f.write(code)
        r = Runner(exe, env)
        try:
            replies = r.ask(opened)
            e = errors(replies)
            if not e or want not in e[0]["message"] or any("page" in m for m in replies):
                failures.append(f"{code!r}: {replies}")
        finally:
            r.close()
        if r.p.returncode != 1:
            failures.append(f"{code!r}: exit {r.p.returncode}, not 1")

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    print("integration page runner: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
