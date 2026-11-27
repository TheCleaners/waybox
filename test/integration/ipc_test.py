#!/usr/bin/env python3
"""Integration test for the scripting socket (on unless <waybox><ipc enabled="no"/>).

1. The socket is there, only for the user (0600).
2. A subscriber gets events: a window opening (new, focus), a desktop
   switch, a window closing, a reload.
3. waybox-msg: get_windows lists the window (focused), get_desktops the
   current one, actions run (GoToDesktop, Close by app_id), an unknown
   action is an error; programs Waybox starts get WAYBOX_SOCK.
4. With <ipc enabled="no"/>, there's no socket.

Skips (exit 77) without foot, or a headless display.

5. A request sent just before closing the connection still runs; actions,
   bell previews' commands and restart_program are refused while the
   screen is locked; and turning the socket off with
   a Reconfigure sent through the socket itself doesn't crash Waybox.

6. Limits and framing: a request's "id" comes back in its reply (errors
   too); many requests sent at once are all answered, in order; a request
   longer than 64 KiB is refused before it runs, and its client closed; a
   subscription with an unknown event subscribes to nothing; past 64
   clients, one more is closed at once and the others still work.

Usage: ipc_test.py <waybox> <waybox-msg> <lock_client>
"""
import json
import os
import queue
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import wbtest

SKIP = 77


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, msg, lock_bin = sys.argv[1:]
    if shutil.which("foot") is None:
        print("SKIP: foot not found", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-ipc-", dir="/tmp")
    failures = []

    def session(tag, ipc, steps):
        cfg = os.path.join(workdir, f"cfg-{tag}")
        os.makedirs(os.path.join(cfg, "waybox"))
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <desktops><number>3</number></desktops>
  <waybox>{'' if ipc else '<ipc enabled="no"/>'}</waybox>
</openbox_config>
""")
        runtime = os.path.join(workdir, f"run-{tag}")
        os.makedirs(runtime, 0o700)
        env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                   XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
                   WB_MENU_XML="", ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
        for v in ("WAYLAND_DISPLAY", "DISPLAY", "WAYBOX_SOCK"):
            env.pop(v, None)
        lp = os.path.join(workdir, f"waybox-{tag}.log")
        log = open(lp, "w")
        # The autostart script (-s) gets WAYBOX_SOCK: the socket opens first.
        autostart = os.path.join(workdir, f"autostart-{tag}")
        proc = subprocess.Popen([waybox, "--debug", "-s",
                                 f"sh -c 'echo \"$WAYBOX_SOCK\" > {autostart}'"],
                                env=env, stdout=log, stderr=subprocess.STDOUT)
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
        cenv = dict(env, WAYLAND_DISPLAY=sock)
        children = []
        try:
            steps(cenv, runtime, sock, proc, children)
        except (RuntimeError, OSError, queue.Empty) as e:
            failures.append(f"{tag}: {e or 'timed out'}")
        finally:
            for c in children:
                if c.poll() is None:
                    c.kill()
            time.sleep(0.3)
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
            log.close()
            if proc.returncode not in (0, None):
                failures.append(f"{tag}: waybox exit status {proc.returncode}")
            if any(s in open(lp).read() for s in ("Sanitizer", "Assertion", "runtime error")):
                failures.append(f"{tag}: sanitizer/assert output")

    def run_msg(cenv, *args):
        r = subprocess.run([msg, *args], env=cenv, capture_output=True, text=True, timeout=10)
        return r.returncode, r.stdout.strip(), r.stderr.strip()

    def with_ipc(cenv, runtime, sock, proc, children):
        path = os.path.join(runtime, f"waybox-{sock}.sock")
        if not os.path.exists(path):
            raise RuntimeError("no scripting socket")
        if stat.S_IMODE(os.stat(path).st_mode) != 0o600:
            failures.append(f"the socket is mode {oct(os.stat(path).st_mode)}, not 0600")
        sub = socket.socket(socket.AF_UNIX)
        sub.connect(path)
        sub.sendall(b'{"request":"subscribe","events":["window","desktop","config"]}\n')
        events = queue.Queue()

        def reader():
            buf = b""
            while True:
                try:
                    data = sub.recv(4096)
                except OSError:
                    return
                if not data:
                    return
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    events.put(json.loads(line))
        threading.Thread(target=reader, daemon=True).start()
        if not events.get(timeout=5).get("ok"):
            raise RuntimeError("subscribe wasn't acknowledged")

        def expect(pred, what, timeout=5.0):
            end = time.time() + timeout
            while time.time() < end:
                try:
                    ev = events.get(timeout=max(0.01, end - time.time()))
                except queue.Empty:
                    break
                if pred(ev):
                    return ev
            failures.append(f"no event: {what}")
            return None

        cenv2 = dict(cenv, WAYBOX_SOCK=path)
        children.append(subprocess.Popen(["foot", "-a", "ipctest"], env=cenv,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        expect(lambda e: e.get("event") == "window" and e.get("change") == "new"
               and e["window"]["app_id"] == "ipctest", "window new")
        expect(lambda e: e.get("event") == "window" and e.get("change") == "focus"
               and e["window"]["app_id"] == "ipctest", "window focus")
        time.sleep(0.3)
        rc, out, err = run_msg(cenv2, "get_windows")
        wins = json.loads(out)["windows"] if rc == 0 else []
        if not any(w["app_id"] == "ipctest" and w["focused"] and w["id"] for w in wins):
            failures.append(f"get_windows didn't list the focused window: {out or err}")

        # A second window, focused: Close by app_id must pick the other one.
        children.append(subprocess.Popen(["foot", "-a", "second"], env=cenv,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        expect(lambda e: e.get("event") == "window" and e.get("change") == "focus"
               and e["window"]["app_id"] == "second", "second window focus")
        rc, out, err = run_msg(cenv2, "action", "Close", "window=app_id:ipctest")
        if rc != 0:
            failures.append(f"action Close failed: {err}")
        expect(lambda e: e.get("event") == "window" and e.get("change") == "close"
               and e["window"]["app_id"] == "ipctest", "window close")
        time.sleep(0.3)
        rc, out, err = run_msg(cenv2, "get_windows")
        left = [w["app_id"] for w in json.loads(out)["windows"]] if rc == 0 else []
        if left != ["second"]:
            failures.append(f"Close by app_id closed the wrong window: left {left}")

        rc, out, err = run_msg(cenv2, "action", "GoToDesktop", "to=2")
        if rc != 0:
            failures.append(f"action GoToDesktop failed: {err}")
        expect(lambda e: e.get("event") == "desktop" and e["desktop"]["number"] == 2,
               "desktop switch")
        rc, out, err = run_msg(cenv2, "get_desktops")
        current = [d["number"] for d in json.loads(out)["desktops"] if d["current"]] if rc == 0 else []
        if current != [2] or len(json.loads(out)["desktops"]) != 3:
            failures.append(f"get_desktops: {out or err}")


        rc, out, err = run_msg(cenv2, "action", "NoSuchAction")
        if rc == 0 or "unknown action" not in err:
            failures.append(f"an unknown action wasn't an error: {rc} {err}")

        marker = os.path.join(runtime, "sock-env")
        run_msg(cenv2, "action", "Execute", f"command=sh -c 'echo $WAYBOX_SOCK > {marker}'")
        time.sleep(0.8)
        if not os.path.exists(marker) or open(marker).read().strip() != path:
            failures.append("a program started by Waybox didn't get WAYBOX_SOCK")

        # The build's version: as --version reports it, and exported.
        built = subprocess.run([waybox, "--version"], capture_output=True,
                               text=True).stdout.split()[-1]
        rc, out, err = run_msg(cenv2, "get_version")
        if rc != 0 or json.loads(out).get("version") != built:
            failures.append(f"get_version isn't the build's version {built}: {out or err}")
        vmark = os.path.join(runtime, "version-env")
        run_msg(cenv2, "action", "Execute", f"command=sh -c 'echo $WAYBOX_VERSION > {vmark}'")
        time.sleep(0.8)
        if not os.path.exists(vmark) or open(vmark).read().strip() != built:
            failures.append("programs Waybox starts don't get WAYBOX_VERSION")

        proc.send_signal(signal.SIGUSR2)
        expect(lambda e: e.get("event") == "config", "config reload")

        # Fire and forget: write a request and close at once.
        fire = socket.socket(socket.AF_UNIX)
        fire.connect(path)
        fire.sendall(b'{"request":"action","action":"GoToDesktop","to":"3"}\n')
        fire.close()
        expect(lambda e: e.get("event") == "desktop" and e["desktop"]["number"] == 3,
               "desktop switch from a request sent just before closing")

        # Limits and framing.
        def connect():
            # (The listen queue holds 16: while Waybox catches up, a
            # connection is refused for a moment; tried again.)
            for _ in range(100):
                k = socket.socket(socket.AF_UNIX)
                try:
                    k.connect(path)
                    k.settimeout(5)
                    return k
                except BlockingIOError:
                    k.close()
                    time.sleep(0.02)
            raise RuntimeError("couldn't connect to the socket")

        def lines_from(k, n):
            buf, got = b"", []
            while len(got) < n:
                chunk = k.recv(65536)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    got.append(json.loads(line))
            return got

        k = connect()
        k.sendall(b'{"request":"get_version","id":"a1"}\n{"request":"nope","id":7}\n'
                  b'{"request":"get_version"}\n')
        r = lines_from(k, 3)
        if [x.get("id") for x in r] != ["a1", 7, None] or r[1].get("ok") is not False:
            failures.append(f"request ids: {r}")
        # Many at once (more than one turn's worth): all, in order.
        k.sendall(b"".join(f'{{"request":"get_version","id":{i}}}\n'.encode()
                           for i in range(200)))
        r = lines_from(k, 200)
        if [x.get("id") for x in r] != list(range(200)):
            failures.append(f"200 requests at once: {len(r)} answered, "
                            f"ids {[x.get('id') for x in r][:5]}...")
        # A subscription naming an unknown event: none taken.
        k.sendall(b'{"request":"subscribe","events":["desktop","not-an-event"],"id":"s"}\n')
        r = lines_from(k, 1)
        if not r or r[0].get("ok") is not False:
            failures.append(f"an unknown event was accepted: {r}")
        run_msg(cenv2, "action", "GoToDesktop", "to=2")
        k.settimeout(0.8)
        try:
            extra = k.recv(65536)
        except socket.timeout:
            extra = b""
        if b'"event"' in extra:
            failures.append("a failed subscription still subscribed to the event before it")
        k.close()
        # Too long: refused before it runs (a reply saying so, or the
        # connection closed while it's still sending), and it never runs.
        k = connect()
        pad = "x" * (70 * 1024)
        r = []
        try:
            k.sendall(('{"request":"action","action":"GoToDesktop","to":"4","pad":"%s"}\n'
                       % pad).encode())
            r = lines_from(k, 1)
        except OSError:
            pass
        k.close()
        if r and (r[0].get("ok") is not False or "64 KiB" not in r[0].get("error", "")):
            failures.append(f"a 70 KiB request wasn't refused: {r}")
        time.sleep(0.3)
        rc, out, err = run_msg(cenv2, "get_desktops")
        try:
            current = [d["number"] for d in json.loads(out)["desktops"] if d.get("current")]
        except (ValueError, KeyError):
            current = None
        if current == [4]:
            failures.append("a 70 KiB request ran")
        # Just over the limit, its end read with it (in one go): refused all
        # the same, with a reply, and it never runs.
        k = connect()
        head = '{"request":"action","action":"GoToDesktop","to":"4","pad":"'
        line = head + "x" * (66 * 1024 - len(head) - 2) + '"}\n'
        r = []
        try:
            k.sendall(line.encode())
            r = lines_from(k, 1)
        except OSError:
            pass
        k.close()
        if not r or "64 KiB" not in r[0].get("error", ""):
            failures.append(f"a 66 KiB request (its end read) wasn't refused: {r}")
        time.sleep(0.3)
        rc, out, err = run_msg(cenv2, "get_desktops")
        try:
            current = [d["number"] for d in json.loads(out)["desktops"] if d.get("current")]
        except (ValueError, KeyError):
            current = None
        if current == [4]:
            failures.append("a 66 KiB request ran")

        # 64 clients at most: the 65th closed at once; the rest still answer.
        many = [connect() for _ in range(64 - 1)]  # (the subscriber is one)
        extra_client = connect()
        try:
            extra_client.settimeout(2)
            extra_client.sendall(b'{"request":"get_version"}\n')
            refused = extra_client.recv(1) == b""
        except OSError:
            refused = True
        if not refused:
            failures.append("a 65th client was served")
        many[0].sendall(b'{"request":"get_version","id":"still"}\n')
        r = lines_from(many[0], 1)
        if not r or r[0].get("id") != "still":
            failures.append(f"with 64 clients, they stopped answering: {r}")
        for m in many:
            m.close()
        extra_client.close()
        time.sleep(0.3)
        rc, out, err = run_msg(cenv2, "get_version")
        if rc != 0:
            failures.append(f"with the others gone, a new client wasn't served: {err}")

        # Locked: actions are refused.
        locker = subprocess.Popen([lock_bin], env=cenv, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True)
        children.append(locker)
        for _ in range(30):
            if "locked" in (locker.stdout.readline() or ""):
                break
        rc, out, err = run_msg(cenv2, "action", "NextWindow")
        if rc == 0 or "locked" not in err:
            failures.append(f"an action ran while the screen was locked: {rc} {err}")
        # A bell preview's command and restart_program too.
        marker = os.path.join(workdir, "bell-ran")
        rc, out, err = run_msg(cenv2, "raw", json.dumps(
            {"request": "bell", "command": f"touch {marker}"}))
        time.sleep(0.4)
        if os.path.exists(marker) or "locked" not in (out + err):
            failures.append(f"a bell preview's command ran while locked: {out} {err}")
        rc, out, err = run_msg(cenv2, "restart_program", "mako")
        if "locked" not in (out + err):
            failures.append(f"restart_program wasn't refused while locked: {out} {err}")
        locker.stdin.write("unlock\n")
        locker.stdin.flush()
        time.sleep(0.5)
        sub.close()

        # The socket turned off by a Reconfigure sent through it.
        rc_path = os.path.join(cenv["XDG_CONFIG_HOME"], "waybox", "rc.xml")
        text = open(rc_path).read().replace("<waybox>", '<waybox><ipc enabled="no"/>', 1)
        open(rc_path, "w").write(text)
        run_msg(cenv2, "action", "Reconfigure")
        time.sleep(0.8)
        if proc.poll() is not None:
            raise RuntimeError(f"waybox died turning the socket off ({proc.returncode})")
        if os.path.exists(path):
            failures.append("the socket is still there after turning it off")
    session("on", True, with_ipc)
    got = open(os.path.join(workdir, "autostart-on")).read().strip() \
        if os.path.exists(os.path.join(workdir, "autostart-on")) else None
    if not got or not got.endswith(".sock"):
        failures.append(f"the autostart script didn't get WAYBOX_SOCK: {got!r}")

    def without(cenv, runtime, sock, proc, children):
        if os.path.exists(os.path.join(runtime, f"waybox-{sock}.sock")):
            failures.append("a socket with <ipc enabled=\"no\"/>")
        rc, out, err = run_msg(cenv, "get_windows")
        if rc == 0:
            failures.append("waybox-msg succeeded without a socket")
    session("off", False, without)

    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration ipc: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
