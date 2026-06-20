#!/usr/bin/env python3
"""Headless integration test for the native root menu.

Binds a Root right-click to ShowMenu (root-menu), defines a menu.xml whose root
menu has a top-level entry that touches a marker file plus a submenu, then:
  - right-clicks the empty desktop to open the menu (ShowMenu -> MenuWidget,
    which renders panels via Cairo/SceneCanvas),
  - moves the pointer over the submenu item (exercises hover -> open_submenu),
  - moves over and left-clicks the top entry (pointer grab -> menu_item_at ->
    take_actions -> run_action(Execute)).
The marker file appearing proves the whole open/render/grab/select path, all
under ASan. Skips (exit 77) when wlrctl or a headless display are unavailable.

Usage: menu_test.py <waybox-binary>
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

SKIP = 77


def need(tool):
    if shutil.which(tool) is None:
        print(f"SKIP: {tool} not found", file=sys.stderr)
        sys.exit(SKIP)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    need("wlrctl")

    workdir = tempfile.mkdtemp(prefix="waybox-menu-")
    marker = os.path.join(workdir, "chosen")
    cfg = os.path.join(workdir, "waybox")
    os.makedirs(cfg, exist_ok=True)
    with open(os.path.join(cfg, "rc.xml"), "w") as f:
        f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <mouse>
    <context name="Root">
      <mousebind button="Right" action="Press">
        <action name="ShowMenu"><command>root-menu</command></action>
      </mousebind>
    </context>
  </mouse>
</openbox_config>
""")
    with open(os.path.join(cfg, "menu.xml"), "w") as f:
        f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu xmlns="http://openbox.org/3.4/menu">
  <menu id="root-menu" label="Waybox">
    <item label="Touch Marker">
      <action name="Execute"><command>touch {marker}</command></action>
    </item>
    <separator/>
    <menu id="sub" label="A Submenu">
      <item label="Nested">
        <action name="Execute"><command>true</command></action>
      </item>
    </menu>
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
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        sys.exit(SKIP)

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

    rc = 0
    try:
        # Open the menu at the desktop (Root context, no windows). After the
        # overshoot-to-origin the pointer is at a known spot, so the menu opens
        # with its top-left at (200, 200) and later relative moves are exact.
        move_to(200, 200)
        r = subprocess.run(["wlrctl", "pointer", "click", "right"], env=cenv,
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"wlrctl right-click failed: {r.stderr.strip()}", file=sys.stderr)
            rc = 1
        time.sleep(0.3)

        # Hover down onto the submenu row (exercises hover -> open_submenu /
        # render), then back up into the first entry row and left-click it. The
        # +8 x keeps the pointer inside the panel (off the left border column).
        move(8, 40)      # ~row 2: the "A Submenu" item
        time.sleep(0.25)
        move(0, -28)     # back up to ~row 0: "Touch Marker"
        time.sleep(0.25)
        subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv,
                       capture_output=True)

        for _ in range(20):
            if os.path.exists(marker):
                break
            time.sleep(0.1)
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            rc = 1
        log.close()

    contents = open(log_path).read()
    if any(m in contents for m in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free", "heap-use-after-free")):
        print("FAIL: sanitizer/assert output", file=sys.stderr)
        sys.stderr.write(contents)
        return 1
    if not os.path.exists(marker):
        print("FAIL: selecting a menu entry did not run its Execute action",
              file=sys.stderr)
        sys.stderr.write(contents)
        return 1

    shutil.rmtree(workdir, ignore_errors=True)
    print("integration menu: OK")
    return rc


if __name__ == "__main__":
    sys.exit(main())
