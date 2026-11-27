#!/usr/bin/env python3
"""Integration test: a Lua page's list { }, sections { } and font { } in wbconf.

A trusted page with a list (a, b, c) and suggestions (x, a): moved,
removed and added to as its buttons do (an add of a name already in it
ignored), its on_change given a table each time (said as a problem with
the count), the change listed as not saved, and Save & Apply has the page
write the names in their new order.
Its sections (one, "a=1", with t=1): one added (urgency=high) and its t
set, one added from a start ("app-name=") and renamed, moved up, the
first's t emptied: saved in that order, the emptied field left out.
Its fonts: one written as fontconfig names (a fallback and antialias=true
kept when the family, weight and slant change), one as Pango writes it
(its size changed, its weight back to the font's own, then + twice, each
followed by the field losing focus: a whole point each).
A font starting with monospace families only (monospace = true): a
proportional family searched for isn't found; with the toggle off, it
is; on again, a monospace one is (families found with fc-list; that
part skipped without both kinds).
A program bound to a key (Kind=binding) has its page too: its change
listed as not saved, and saved.
Skips (exit 77) without a headless display.

Usage: wbconf_page_list_test.py <waybox> <wbconf> <schema-dir> <wbconf-lua>
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77

INTEGRATION = """[Integration]
Name=Lists
Category=Notifications
Exec=true
Page=lists.lua
Files=~/.config/lists/config
"""

KEYED = """[Integration]
Name=Keyed
Category=Launcher
Exec=true --keyed
Kind=binding
Key=W-k
Page=keyed.lua
Files=~/.config/keyed/config
"""

KEYED_PAGE = r'''
page { text { "word", label = "Word", value = wb.read("~/.config/keyed/config") or "" } }
function on_save(v) wb.write("~/.config/keyed/config", v.word) end
'''

PAGE = r'''
page { list { "items", label = "Items", value = { "a", "b", "c" },
              choices = { "x", { "a", "The first" } } },
       sections { "rules", label = "Rules", value = { { name = "a=1", values = { t = 1 } } },
                  choices = { "urgency=high", { "app-name=", "A program" } },
                  fields = { number { "t", label = "Time" },
                             choice { "h", label = "Shown", choices = { "0", "1" } } } },
       font { "mono", label = "Mono", format = "fontconfig", hint = "monospace 10",
              value = "Iosevka,Noto Color Emoji:size=12:antialias=true" },
       font { "ui", label = "UI", value = "Inter Bold 11" },
       font { "term", label = "Terminal", monospace = true, hint = "monospace 10" } }
function on_change(key, value)
  local want = (key == "items" or key == "rules") and "table" or "string"
  if type(value) ~= want then error(key .. ": not a " .. want .. ": " .. type(value)) end
  if key == "items" then wb.problem("items", #value .. " items") end
end
function on_save(v)
  local rules = {}
  for _, r in ipairs(v.rules) do
    rules[#rules + 1] = r.name .. "(" .. (r.values.t or "") .. (r.values.h or "") .. ")"
  end
  wb.write("~/.config/lists/config", table.concat(v.items, ",") .. ";" ..
      table.concat(rules, ",") .. ";" .. v.mono .. ";" .. v.ui)
end
'''


def unique_family(monospace):
    """A monospace family (fc-list's spacing 100 or 90) or a proportional
    one (with no spacing: fontconfig marks only the others) whose name no
    other family's has in it: a search finds it alone."""
    def families(pattern):
        try:
            out = subprocess.run(["fc-list", pattern, "family"], capture_output=True,
                                 text=True, timeout=20).stdout
        except (OSError, subprocess.TimeoutExpired):
            return set()
        return {l.split(",")[0].strip() for l in out.splitlines() if l.strip()}
    names = families(":")
    mono = families(":spacing=100") | families(":spacing=90")
    pool = mono if monospace else names - mono
    for cand in sorted(pool):
        if cand and not any(cand.lower() in n.lower() for n in names if n != cand) and \
                all(ord(ch) < 128 for ch in cand) and ";" not in cand:
            return cand
    return None


def variable_one_width():
    """A variable font with a weight axis and no width axis (fc-list gives its
    width as one number, its weight as a range): Cascadia Code, Inter
    Variable. None without one."""
    try:
        out = subprocess.run(["fc-list", ":variable=True", "family", "weight", "width"],
                             capture_output=True, text=True, timeout=20).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    for line in sorted(out.splitlines()):
        parts = dict(p.split("=", 1) for p in line.split(":")[1:] if "=" in p)
        family = line.split(":")[0].split(",")[0].strip()
        if parts.get("weight", "").startswith("[") and "[" not in parts.get("width", "[") \
                and family.isascii() and ";" not in family:
            return family
    return None


WEIGHTS = [100, 200, 300, 400, 500, 600, 700, 800, 900]


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        sys.exit(2)
    waybox, wbconf, schema, runner = sys.argv[1:]
    workdir = tempfile.mkdtemp(prefix="waybox-pagelist-", dir="/tmp")
    home = os.path.join(workdir, "home")
    cfg = os.path.join(home, ".config")
    os.makedirs(os.path.join(cfg, "waybox"))
    with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
        f.write('<?xml version="1.0"?>\n<openbox_config xmlns="http://openbox.org/3.4/rc"/>\n')
    share = os.path.join(workdir, "share")
    packaged = os.path.join(share, "waybox", "integrations")
    os.makedirs(packaged)
    with open(os.path.join(packaged, "lists.integration"), "w") as f:
        f.write(INTEGRATION)
    with open(os.path.join(packaged, "lists.lua"), "w") as f:
        f.write(PAGE)
    with open(os.path.join(packaged, "keyed.integration"), "w") as f:
        f.write(KEYED)
    with open(os.path.join(packaged, "keyed.lua"), "w") as f:
        f.write(KEYED_PAGE)
    for n in os.listdir(packaged):
        os.chmod(os.path.join(packaged, n), 0o444)
    os.chmod(packaged, 0o555)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman", HOME=home,
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
               XDG_DATA_DIRS=share, ASAN_OPTIONS="detect_leaks=0")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log = open(os.path.join(workdir, "waybox.log"), "w")
    proc = subprocess.Popen([waybox], env=env, stdout=log, stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)
    sock = None
    for _ in range(50):
        time.sleep(0.1)
        if proc.poll() is not None:
            break
        socks = [n for n in os.listdir(runtime)
                 if n.startswith("wayland-") and not n.endswith(".lock")]
        if socks:
            sock = socks[0]
            break
    if sock is None:
        log.close()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)
    steps = ["luapage:lists", "luaset:lists.items=@down:0", "luaset:lists.items=@remove:2",
             "luaset:lists.items=@add:x", "luaset:lists.items=@add:a", "luaset:lists.items=@up:0",
             "luaset:lists.rules=@add:urgency=high", "luaset:lists.rules=@set:1:t=5",
             "luaset:lists.rules=@add:app-name=", "luaset:lists.rules=@rename:2:app-name=foot",
             "luaset:lists.rules=@up:2", "luaset:lists.rules=@set:0:t=",
             "luaset:lists.mono=@family:DejaVu Sans Mono", "luaset:lists.mono=@weight:700",
             "luaset:lists.mono=@italic:on", "luaset:lists.ui=@size:13",
             "luaset:lists.ui=@weight:0", "luaset:lists.ui=@size-step:up",
             "luaset:lists.ui=@size-update:", "luaset:lists.ui=@size-step:up",
             "luaset:lists.ui=@size-update:",
             "luaset:keyed.word=hello", "luapage:keyed",
             "luapage:lists", "changes", "save", "luapage:lists"]
    # A variable font with no width axis: its weights in between, one width.
    variable = variable_one_width()
    if variable:
        steps += [f"luaset:lists.ui=@family:{variable}", "luaset:lists.ui=@offered:ui"]
    else:
        print("note: no variable font without a width axis: not tried", file=sys.stderr)
    mono_family, prop_family = unique_family(True), unique_family(False)
    term_final = ""
    if mono_family and prop_family:
        at = steps.index("luaset:keyed.word=hello")
        steps[at:at] = [f"luaset:lists.term=@family-search:{prop_family}",
                        "luapage:lists",
                        "luaset:lists.term=@mono:off",
                        f"luaset:lists.term=@family-search:{prop_family}",
                        "luapage:lists",
                        "luaset:lists.term=@mono:on",
                        f"luaset:lists.term=@family-search:{mono_family}"]
        term_final = f"{mono_family} 10"
    else:
        print("note: no unique monospace and proportional families: the filter "
              "not tried", file=sys.stderr)
    wenv = dict(env, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                WAYBOX_PID=str(proc.pid), WBCONF_LUA=runner,
                WBCONF_TEST_TOC=";".join(steps))
    failures = []
    app = subprocess.Popen([wbconf], env=wenv, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True)
    wbtest.reap_on_exit(app)
    time.sleep(0.8 * (len(steps) + 2) + 1.5)
    if app.poll() is not None:
        _, err = app.communicate()
        failures.append(f"wbconf ended ({app.returncode}): {err[-800:]}")
    else:
        app.kill()
        _, err = app.communicate()
    lines = err.splitlines()
    d = [l.split("|")[1:] for l in lines if l.startswith("luapage-dump:lists")]
    if mono_family and prop_family and len(d) == 5:
        # Searched with the toggle on: not found; off: picked.
        if "term=" not in d[1]:
            failures.append(f"{prop_family} picked while only monospace ones are listed: "
                            f"{[x for x in d[1] if x.startswith('term=')]}")
        if f"term={prop_family} 10" not in d[2]:
            failures.append(f"with the toggle off, {prop_family} not picked: {d[2]}")
        d = [d[0]] + d[3:]
    rules0 = 'rules=[{"name":"a=1","values":{"t":"1"}}]'
    fonts0 = ["mono=Iosevka,Noto Color Emoji:size=12:antialias=true", "ui=Inter Bold 11",
              "term="]
    fonts1 = ["mono=DejaVu Sans Mono,Noto Color Emoji:size=12:weight=bold:slant=italic:"
              "antialias=true", "ui=Inter 15", "term=" + term_final]
    rules1 = ('rules=[{"name":"a=1","values":{}},{"name":"app-name=foot","values":{}},'
              '{"name":"urgency=high","values":{"t":"5"}}]')
    if len(d) != 3:
        failures.append(f"{len(d)} dumps, not 3: {lines[-10:]}")
    else:
        if d[0] != ["items=a\\nb\\nc", rules0] + fonts0:
            failures.append(f"as given: {d[0]}")
        # down:0 -> b a c; remove:2 -> b a; add x -> b a x; add a: no;
        # up:0 -> no (the first): b a x.
        if d[1] != ["items=b\\na\\nx!3 items", rules1] + fonts1:
            failures.append(f"after the changes: {d[1]}")
        if d[2] != d[1]:
            failures.append(f"after saving: {d[2]}")
    changes = [l for l in lines if l.startswith("changes-dump:")]
    if not changes or "Items" not in changes[0]:
        failures.append(f"the change not listed: {changes}")
    if variable:
        offered = [l for l in lines if l.startswith("font-offered:ui|")]
        got = dict(p.split("=", 1) for p in offered[0].split("|")[1:]) if offered else {}
        weights = [int(w) for w in got.get("weights", "").split(",") if w and w != "0"]
        lo = WEIGHTS.index(min(weights)) if weights else 0
        if got.get("widths") != "0,100" or got.get("width") != "off":
            failures.append(f"{variable}: widths it lacks offered: {offered}")
        elif len(weights) < 3 or weights != WEIGHTS[lo:lo + len(weights)]:
            failures.append(f"{variable}: its weights not all those in between: {offered}")
    keyed = [l for l in lines if l.startswith("luapage-dump:keyed")]
    if keyed != ["luapage-dump:keyed|word=hello"]:
        failures.append(f"the key-bound program's page: {keyed}")
    if not changes or "Word" not in changes[0]:
        failures.append(f"its change not listed: {changes}")
    kconf = os.path.join(cfg, "keyed", "config")
    if not os.path.exists(kconf) or open(kconf).read() != "hello":
        failures.append("the key-bound program's page wasn't saved")
    conf = os.path.join(cfg, "lists", "config")
    if not os.path.exists(conf) or \
            open(conf).read() != "b,a,x;a=1(),app-name=foot(),urgency=high(5);" + \
            fonts1[0][5:] + ";" + fonts1[1][3:]:
        failures.append(f"the file as saved: {open(conf).read() if os.path.exists(conf) else None}")
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    log.close()
    os.chmod(packaged, 0o755)
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration wbconf page list: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
