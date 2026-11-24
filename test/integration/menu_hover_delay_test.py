#!/usr/bin/env python3
"""Integration test for <waybox><menu hoverDelay="ms"/>.

The root menu has a submenu entry. With hoverDelay="800":

1. Resting on the entry for 0.3 s doesn't open the submenu yet.
2. After 1.2 s it is open.
3. Moving the pointer within the entry keeps it open (it isn't closed and
   rebuilt on every motion): it opened once.
4. Passing over the entry quickly (less than the delay) doesn't open it.

With hoverDelay="0" it opens at once.

Skips (exit 77) without wlrctl, or a headless display.

Usage: menu_hover_delay_test.py <waybox-binary>
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

SKIP = 77


def run(waybox, delay):
    """Returns (log text, events) for one session with this hoverDelay."""
    workdir = tempfile.mkdtemp(prefix="waybox-hover-")
    cfg = os.path.join(workdir, "waybox")
    os.makedirs(cfg, exist_ok=True)
    with open(os.path.join(cfg, "rc.xml"), "w") as f:
        f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <mouse>
    <context name="Root">
      <mousebind button="Right" action="Press">
        <action name="ShowMenu"><command>root-menu</command></action>
      </mousebind>
    </context>
  </mouse>
  <waybox><menu submenuOpen="hover" hoverDelay="{delay}"/></waybox>
</openbox_config>
""")
    with open(os.path.join(cfg, "menu.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu xmlns="http://openbox.org/3.4/menu">
  <menu id="root-menu" label="Waybox">
    <item label="First"><action name="Execute"><command>true</command></action></item>
    <separator/>
    <menu id="sub" label="A Submenu">
      <item label="Nested"><action name="Execute"><command>true</command></action></item>
    </menu>
    <item label="Last"><action name="Execute"><command>true</command></action></item>
  </menu>
</openbox_menu>
""")

    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, exist_ok=True)
    os.chmod(runtime, 0o700)
    # The menu renders text, so the compositor pulls in Pango/fontconfig, whose
    # process-global font caches LeakSanitizer would flag at exit. Suppress
    # those library caches (as render_test does) while still catching real leaks.
    suppfile = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "lsan.supp")
    env = dict(
        os.environ,
        WLR_BACKENDS="headless",
        WLR_RENDERER="pixman",
        XDG_CONFIG_HOME=workdir,
        XDG_RUNTIME_DIR=runtime,
        ASAN_OPTIONS="detect_leaks=1:exitcode=99",
        LSAN_OPTIONS=f"suppressions={suppfile}:print_suppressions=0",
    )

    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)

    display = None
    for _ in range(50):
        time.sleep(0.1)
        if proc.poll() is not None:
            break
        for name in os.listdir(runtime):
            if name.startswith("wayland-") and not name.endswith(".lock"):
                display = name
                break
        if display:
            break
    if display is None:
        log.close()
        sys.stderr.write(open(log_path).read())
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        return "", {"skip": True, "status": None}

    cenv = dict(env, WAYLAND_DISPLAY=display)

    def move(x, y):
        subprocess.run(["wlrctl", "pointer", "move", str(x), str(y)],
                       env=cenv, capture_output=True)

    # wlrctl pointer move is relative; recentre by overshooting to the origin
    # first, then stepping to the target.
    def move_to(x, y):
        move(-5000, -5000)
        time.sleep(0.05)
        move(x, y)
        time.sleep(0.1)


    def opened():
        log.flush()
        return open(log_path).read().count("wb-menu open sub")

    def closed():
        log.flush()
        return open(log_path).read().count("wb-menu close sub")

    events = {}
    try:
        move_to(200, 200)
        subprocess.run(["wlrctl", "pointer", "click", "right"], env=cenv,
                       capture_output=True)
        time.sleep(0.4)
        if delay > 0:
            # 4. Pass over the entry quickly, on to the entry below it.
            move(8, 40)
            time.sleep(0.15)
            move(0, 28)
            time.sleep(1.2)
            events["passed_over"] = opened()
            move(0, -28)        # back onto the submenu entry
            time.sleep(0.3)
            events["early"] = opened()
            time.sleep(0.9)
            events["after_delay"] = opened()
            closes = closed()
            for dx in (4, 4, -4, -4):  # wiggle within the entry
                move(dx, 0)
                time.sleep(0.1)
            time.sleep(0.3)
            events["after_wiggle"] = opened()
            events["closed_by_wiggle"] = closed() - closes
        else:
            move(8, 40)
            time.sleep(0.25)
            events["at_once"] = opened()
    finally:
        log.close()
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
    text = open(log_path).read()
    events["status"] = proc.returncode
    shutil.rmtree(workdir, ignore_errors=True)
    return text, events


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    if shutil.which("wlrctl") is None:
        print("SKIP: wlrctl not found", file=sys.stderr)
        sys.exit(SKIP)
    failures = []
    logs = ""
    text, ev = run(waybox, 800)
    logs += text
    if ev.get("skip"):
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        sys.exit(SKIP)
    if ev["passed_over"] != 0:
        failures.append("passing over the entry quickly opened the submenu")
    if ev["early"] != 0:
        failures.append("the submenu opened before hoverDelay")
    if ev["after_delay"] != 1:
        failures.append(f"the submenu didn't open once after hoverDelay ({ev['after_delay']})")
    if ev["closed_by_wiggle"] != 0:
        failures.append("moving within the entry closed its submenu")
    if ev["after_wiggle"] != 1:
        failures.append(f"moving within the entry reopened the submenu "
                        f"({ev['after_wiggle']} opens)")
    text, ev0 = run(waybox, 0)
    logs += text
    if ev0.get("at_once") != 1:
        failures.append("with hoverDelay 0 the submenu didn't open at once")
    for e in (ev, ev0):
        if e["status"] not in (0, None):
            failures.append(f"waybox exit status {e['status']}")
    if any(s in logs for s in ("Sanitizer", "Assertion", "runtime error",
                               "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(logs[-3000:])
        return 1
    print("integration menu hover delay: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
