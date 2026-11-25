#!/usr/bin/env python3
"""Integration test: the bell's styles, pacing, IPC and Lua.

A headless Waybox with the bell test client's window (app_id bell-test),
filling the screen's top left corner.

1. Styles, through IPC previews (wbconf's Test): the window's middle and
   its edge, from screenshots, while the flash is up:
   - flash="window": the middle changes;
   - flash="border": the edge changes, the middle doesn't;
   - flash="screen": a point outside the window changes;
   - flash="osd": a note (logged), nothing flashed;
   - count=3: three blinks (logged), never closer than 1/3 s.
2. Pacing: 50 rings as fast as IPC takes them: at most 3 flashes in that
   second, the rest let go.
3. waybox-msg subscribe bell: a real ring from the window gives an event
   with the window and what was shown.
4. Lua: a "bell" policy gives the window a red border and silences rings
   from no window; a "bell" event handler sees both rings.
5. (With wbconf's path and schema: only this.) wbconf's Test, with the
   bell set to a border in wbconf and not saved: Waybox flashes a border,
   with the settings on screen, not rc.xml's.
Skips (77) without grim, or a headless display.

Usage: bell_styles_test.py <waybox> <bell_client> <waybox-msg> [<wbconf> <schema-dir>]
"""
import json
import os
import queue
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import wbtest

SKIP = 77

LUA = """
wb.policy("bell", function(w)
  if w then
    return {flash = "border", color = "#ff0000ff", duration = 900, width = 6}
  end
  return false
end)
wb.on("bell", function(w, info)
  wb.log("bell-event [" .. info.app_id .. "]")
end)
"""


class Lines:
    def __init__(self, proc):
        self.proc = proc
        self.q = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.q.put(line.strip())

    def wait_for(self, text, seconds=5):
        end = time.time() + seconds
        while time.time() < end:
            try:
                line = self.q.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if text in line:
                return line
        return None


def main():
    if len(sys.argv) not in (4, 6):
        print(__doc__)
        sys.exit(2)
    waybox, client_bin, msg = sys.argv[1:4]
    wbconf_args = sys.argv[4:]
    if shutil.which("grim") is None:
        print("SKIP: grim not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-bellstyles-", dir="/tmp")
    failures = []

    def start(with_lua):
        cfg = os.path.join(workdir, "config-lua" if with_lua else "config")
        os.makedirs(os.path.join(cfg, "waybox"))
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox><ipc enabled="yes"/><bell flash="window" color="#ffffffff"/></waybox>
</openbox_config>
""")
        if with_lua:
            with open(os.path.join(cfg, "waybox", "init.lua"), "w") as f:
                f.write(LUA)
        runtime = os.path.join(workdir, "run-lua" if with_lua else "run")
        os.makedirs(runtime, 0o700)
        env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                   XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="", XDG_CONFIG_HOME=cfg,
                   ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "")
                   + "exitcode=99")
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("DISPLAY", None)
        log_path = os.path.join(workdir, "waybox-lua.log" if with_lua else "waybox.log")
        log = open(log_path, "w")
        proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                                stderr=subprocess.STDOUT)
        wbtest.reap_on_exit(proc)
        for _ in range(50):
            time.sleep(0.1)
            if proc.poll() is not None:
                break
            socks = [n for n in os.listdir(runtime)
                     if n.startswith("wayland-") and not n.endswith(".lock")]
            if socks:
                cenv = dict(env, WAYLAND_DISPLAY=socks[0])
                client = subprocess.Popen([client_bin], env=cenv, text=True,
                                          stdin=subprocess.PIPE, stdout=subprocess.PIPE)
                wbtest.reap_on_exit(client)
                lines = Lines(client)
                if lines.wait_for("configured") is None:
                    failures.append("the window never showed")
                return proc, log, log_path, cenv, client, lines
        log.close()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)

    def stop(proc, log, client):
        client.kill()
        client.wait()
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
        if proc.returncode not in (0, None):
            failures.append(f"waybox exit status {proc.returncode}")

    def bell(cenv, **fields):
        r = subprocess.run([msg, "raw", json.dumps(dict(request="bell", **fields))],
                           env=cenv, capture_output=True, text=True, timeout=10)
        try:
            return json.loads(r.stdout).get("shown")
        except ValueError:
            return r.stdout + r.stderr

    def pixel(cenv, x, y):
        r = subprocess.run(["grim", "-t", "ppm", "-g", f"{x},{y} 1x1", "-"], env=cenv,
                           capture_output=True, timeout=10)
        return r.stdout[-3:]

    def window_box(cenv):
        r = subprocess.run([msg, "get_windows"], env=cenv, capture_output=True, text=True)
        w = json.loads(r.stdout)["windows"][0]
        return w["x"], w["y"], w["width"], w["height"]

    if wbconf_args:
        wbconf, schema = wbconf_args
        proc, log, log_path, cenv, client, lines = start(False)
        try:
            wenv = dict(cenv, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                        DBUS_SESSION_BUS_ADDRESS="disabled:", GSETTINGS_BACKEND="memory",
                        GIO_USE_VFS="local", NO_AT_BRIDGE="1", WBCONF_SCHEMA_DIR=schema,
                        WAYBOX_PID=str(proc.pid),
                        WBCONF_TEST_SET="waybox.bell.flash=border;waybox.bell.width=12;"
                                        "bell-test=1")
            app = subprocess.Popen([wbconf], env=wenv, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
            wbtest.reap_on_exit(app)
            time.sleep(4)
            alive = app.poll() is None
            if alive:
                app.kill()
            app.wait()
            log.flush()
            text = open(log_path).read()
            if not alive:
                failures.append("wbconf exited")
            if "wb-bell flash border" not in text:
                failures.append("wbconf's Test didn't flash a border (the settings on screen)")
            if "wb-bell ring" in text:
                failures.append("a preview went through as a ring (policy, events)")
        finally:
            stop(proc, log, client)
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        if failures:
            return 1
        shutil.rmtree(workdir, ignore_errors=True)
        print("integration bell (wbconf): OK")
        return 0

    # 1-3: rc.xml's bell, previews, pacing, subscribe.
    proc, log, log_path, cenv, client, lines = start(False)
    try:
        x, y, w, h = window_box(cenv)
        mid = (x + w // 2, y + h // 2)
        edge = (x + 1, y + h // 2)
        outside = (x + w + 200, y + h + 200)
        base = {p: pixel(cenv, *p) for p in (mid, edge, outside)}
        long = dict(color="#ff00ffff", duration=1500)

        def during(**fields):
            shown = bell(cenv, **fields)
            time.sleep(0.25)
            now = {p: pixel(cenv, *p) for p in (mid, edge, outside)}
            time.sleep(1.7)  # gone, and past the pacing
            return shown, now

        shown, now = during(flash="window", **long)
        if shown != "window" or now[mid] == base[mid]:
            failures.append(f"window: {shown}, middle {base[mid]} -> {now[mid]}")
        shown, now = during(flash="border", width=8, **long)
        if shown != "border" or now[edge] == base[edge] or now[mid] != base[mid]:
            failures.append(f"border: {shown}, edge {base[edge]} -> {now[edge]}, "
                            f"middle {base[mid]} -> {now[mid]}")
        shown, now = during(flash="screen", **long)
        if shown != "screen" or now[outside] == base[outside]:
            failures.append(f"screen: {shown}, outside {base[outside]} -> {now[outside]}")
        shown, now = during(flash="osd", duration=1500)
        if shown != "osd" or now[edge] != base[edge]:
            failures.append(f"osd: {shown}")
        log.flush()
        before = open(log_path).read().count("wb-bell blink")
        if bell(cenv, flash="window", duration=60, count=3) != "window":
            failures.append("count=3 didn't flash")
        time.sleep(1.2)
        log.flush()
        blinks = open(log_path).read().count("wb-bell blink") - before
        if blinks != 3:
            failures.append(f"count=3 blinked {blinks} times")
        time.sleep(0.5)

        # 2. A flood: no strobe.
        start_t = time.time()
        shown = [bell(cenv, flash="window", duration=50) for _ in range(50)]
        took = time.time() - start_t
        flashes = sum(1 for s in shown if s == "window")
        allowed = 1 + int(took * 3)  # never more than 3 a second
        if flashes > allowed or flashes < 1 or shown.count("paced") != 50 - flashes:
            failures.append(f"a flood of 50 rings in {took:.2f}s flashed {flashes} times")
        time.sleep(0.5)

        # 3. Subscribed: a real ring from the window.
        sub = subprocess.Popen([msg, "subscribe", "bell"], env=cenv, text=True,
                               stdout=subprocess.PIPE)
        wbtest.reap_on_exit(sub)
        events = Lines(sub)
        time.sleep(0.3)
        client.stdin.write("window\n")
        client.stdin.flush()
        line = events.wait_for('"event"')
        sub.kill()
        sub.wait()
        ev = json.loads(line) if line else {}
        if ev.get("event") != "bell" or ev.get("shown") != "window" or \
                ev.get("window", {}).get("app_id") != "bell-test":
            failures.append(f"bell event: {line}")
    except (OSError, ValueError, KeyError, IndexError) as e:
        failures.append(f"{type(e).__name__}: {e}")
    finally:
        stop(proc, log, client)

    # 4. Lua: a policy and an event.
    proc, log, log_path, cenv, client, lines = start(True)
    try:
        x, y, w, h = window_box(cenv)
        edge = (x + 2, y + h // 2)
        mid = (x + w // 2, y + h // 2)
        b_edge, b_mid = pixel(cenv, *edge), pixel(cenv, *mid)
        client.stdin.write("window\n")
        client.stdin.flush()
        lines.wait_for("rang")
        time.sleep(0.25)
        n_edge, n_mid = pixel(cenv, *edge), pixel(cenv, *mid)
        if n_edge != b"\xff\x00\x00" or n_mid != b_mid:
            failures.append(f"the policy's red border: edge {b_edge} -> {n_edge}, "
                            f"middle {b_mid} -> {n_mid}")
        time.sleep(1.2)
        client.stdin.write("none\n")
        client.stdin.flush()
        lines.wait_for("rang")
        time.sleep(0.5)
        log.flush()
        text = open(log_path).read()
        if "wb-bell flash output" in text:
            failures.append("the policy's false didn't silence a ring from no window")
        if "bell-event [bell-test]" not in text or "bell-event []" not in text:
            failures.append("the bell event handler didn't see both rings")
    except (OSError, ValueError, KeyError, IndexError) as e:
        failures.append(f"{type(e).__name__}: {e}")
    finally:
        stop(proc, log, client)

    for lp in ("waybox.log", "waybox-lua.log"):
        p = os.path.join(workdir, lp)
        if os.path.exists(p) and any(s in open(p).read()
                                     for s in ("Sanitizer", "Assertion", "runtime error")):
            failures.append(f"sanitizer/assert output in {lp}")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration bell styles: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
