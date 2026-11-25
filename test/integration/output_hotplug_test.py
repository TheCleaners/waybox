#!/usr/bin/env python3
"""Integration test: an output disappearing under a layer-shell panel.

Runs waybox on the X11 backend inside Xvfb with two outputs (two X windows)
and a panel client (layer_client.c) with a bar on each, redrawing every frame.
Closing one output's X window destroys that output, as unplugging a monitor
or a TV dropping off on a VT switch does. Waybox must:

  1. survive (it used to crash in arrange_layers() on the freed output),
  2. close the bar on the gone output (the client sees "closed"),
  3. keep serving the bar on the remaining output, and exit cleanly.

Skips (exit 77) without Xvfb or python3-xlib.

Usage: output_hotplug_test.py <waybox-binary> <layer-client-binary>
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


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, layer_client = sys.argv[1], sys.argv[2]
    if shutil.which("Xvfb") is None:
        print("SKIP: Xvfb not found", file=sys.stderr)
        sys.exit(SKIP)
    try:
        from Xlib import X, display as xdisplay, protocol
    except ImportError:
        print("SKIP: python3-xlib not found", file=sys.stderr)
        sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-hp-", dir="/tmp")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    failures = []

    # A private X server; -displayfd reports the display number it picked.
    rfd, wfd = os.pipe()
    xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(wfd), "-screen", "0",
                             "2200x900x24", "-nolisten", "tcp", "-noreset"],
                            pass_fds=[wfd], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    os.close(wfd)
    num = os.read(rfd, 16).decode().strip()
    os.close(rfd)
    if not num:
        xvfb.kill()
        print("SKIP: Xvfb did not start", file=sys.stderr)
        sys.exit(SKIP)
    x_display = f":{num}"

    # wlroots' X11 backend only looks these atoms up if they already exist;
    # on a fresh Xvfb nothing has created them, so it would ignore the
    # WM_DELETE_WINDOW we send below. Create them first (-noreset keeps them).
    # Retried briefly: with many tests reserving X displays at once, the
    # first connection can be refused for a moment.
    d = None
    for _ in range(30):
        try:
            d = xdisplay.Display(x_display)
            break
        except Exception:  # Xlib raises several kinds here
            time.sleep(0.1)
    if d is None:
        xvfb.kill()
        print("SKIP: could not connect to Xvfb", file=sys.stderr)
        sys.exit(SKIP)
    for atom in ("WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_WM_NAME",
                 "UTF8_STRING", "_NET_WM_PID"):
        d.intern_atom(atom)
    d.sync()

    env = dict(os.environ, DISPLAY=x_display, WLR_BACKENDS="x11",
               WLR_RENDERER="pixman", WLR_X11_OUTPUTS="2",
               XDG_RUNTIME_DIR=runtime, XDG_CONFIG_HOME=workdir,
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(proc)

    client = None
    try:
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
            failures.append("waybox did not come up on the X11 backend")
            return report(failures, log, log_path, workdir)

        client = subprocess.Popen([layer_client], env=dict(env, WAYLAND_DISPLAY=sock),
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                  text=True)
        time.sleep(1.5)

        # The two output windows are top-level children of the Xvfb root.
        root = d.screen().root
        windows = [w for w in root.query_tree().children
                   if w.get_attributes().map_state == X.IsViewable]
        if len(windows) < 2:
            failures.append(f"expected 2 output windows, found {len(windows)}")
            return report(failures, log, log_path, workdir)
        victim = windows[-1]
        proto = d.intern_atom("WM_PROTOCOLS")
        delete = d.intern_atom("WM_DELETE_WINDOW")
        victim.send_event(protocol.event.ClientMessage(
            window=victim, client_type=proto,
            data=(32, [delete, X.CurrentTime, 0, 0, 0])), event_mask=X.NoEventMask)
        d.flush()
        time.sleep(2.0)  # the panel keeps redrawing meanwhile

        if proc.poll() is not None:
            failures.append(f"waybox died after the output was destroyed (exit {proc.returncode})")
        client.send_signal(signal.SIGTERM)
        out = client.communicate(timeout=5)[0].split()
        if out.count("configured") < 2:
            failures.append(f"the panel was not configured on both outputs ({out})")
        if out.count("closed") != 1:
            failures.append(f"expected exactly one bar closed, got {out.count('closed')} ({out})")
    finally:
        if client is not None and client.poll() is None:
            client.kill()
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                failures.append("waybox did not exit on SIGTERM")
        xvfb.terminate()
        xvfb.wait()
    return report(failures, log, log_path, workdir)


def report(failures, log, log_path, workdir):
    log.close()
    contents = open(log_path).read()
    if any(m in contents for m in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-4000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration output hotplug: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
