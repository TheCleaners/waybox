#!/usr/bin/env python3
"""Integration test for rc.xml <waybox><startup> programs.

1. At startup: enabled programs start, a disabled one doesn't; one that
   exits cleanly isn't started again; one that crashes is, after 1 s
   (restart="no" turns that off).
2. Reconfigure with a changed list: a removed program is stopped (its
   process is gone), one enabled now starts, an unchanged one-shot isn't
   run again, a changed command is restarted. A removed program that
   exited cleanly but left something running in its process group (as
   waypaper --restore leaves swaybg) has that stopped; one ignoring
   SIGTERM is killed 3 s later, its exited leader notwithstanding. A
   leader kept for what it left running is reaped once that has ended
   (no zombie left behind).
3. A program killed is started again.
4. When Waybox exits, the programs it started end, and what a program
   that exited left running.
5. Without <startup>, nothing is started.

Usage: startup_programs_test.py <waybox>
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import wbtest

SKIP = 77


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    # A zombie still answers kill -0.
    state = process_state(pid)
    return bool(state) and not state.startswith("Z")


def process_state(pid):
    """A process's state letter(s): Linux's /proc where there is one, else
    ps (the BSDs: FreeBSD's procfs, when mounted, has no stat file); "" if
    it's gone."""
    try:
        with open(f"/proc/{pid}/stat") as f:
            return f.read().rsplit(") ", 1)[1][0]
    except FileNotFoundError:
        if os.path.exists("/proc/self/stat"):
            return ""
    except (OSError, IndexError):
        return ""
    r = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)], capture_output=True, text=True)
    return r.stdout.strip()


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    workdir = tempfile.mkdtemp(prefix="waybox-startup-", dir="/tmp")
    cfg = os.path.join(workdir, "config")
    os.makedirs(os.path.join(cfg, "waybox"))
    rc_path = os.path.join(cfg, "waybox", "rc.xml")
    once_log = os.path.join(workdir, "once.log")
    left_pid = os.path.join(workdir, "left.pid")
    kept_pid = os.path.join(workdir, "kept.pid")
    stubborn_pid = os.path.join(workdir, "stubborn.pid")

    def write_rc(programs):
        body = "\n".join(
            f'    <program name="{n}"{extra}>{cmd}</program>' for n, cmd, extra in programs or [])
        startup = f"<startup>\n{body}\n  </startup>" if programs is not None else ""
        with open(rc_path, "w") as f:
            f.write(f"""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox>{startup}</waybox>
</openbox_config>
""")

    first = [
        ("sleeper", "exec sleep 1000", ""),
        ("later", "exec sleep 1001", ' enabled="no"'),
        ("once", f"echo ran &gt;&gt; {once_log}", ""),
        ("crashy", "exit 3", ""),
        ("quitter", "exit 4", ' restart="no"'),
        # Exit cleanly, leaving a process in their group.
        ("restorer", f"sleep 1003 &amp; echo $! &gt; {left_pid}", ""),
        ("keeper", f"sleep 1004 &amp; echo $! &gt; {kept_pid}", ""),
        # Leaves one that ignores SIGTERM; and one that ends by itself.
        ("stubborn", f"(trap '' TERM; exec sleep 1005) &amp; echo $! &gt; {stubborn_pid}", ""),
        ("brief", "sleep 2 &amp;", ""),
    ]
    write_rc(first)
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="",
               WB_MENU_XML="", ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)
    log_path = os.path.join(workdir, "waybox.log")
    failures = []

    def launch(tag):
        lp = os.path.join(workdir, f"waybox-{tag}.log")
        log = open(lp, "w")
        proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                                stderr=subprocess.STDOUT)
        wbtest.reap_on_exit(proc)
        for _ in range(50):
            time.sleep(0.1)
            if proc.poll() is not None:
                break
            if any(n.startswith("wayland-") and not n.endswith(".lock")
                   for n in os.listdir(runtime)):
                return proc, log, lp
        log.close()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)

    def stop(proc, log):
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                failures.append("waybox didn't exit")
        log.close()
        if proc.returncode not in (0, None):
            failures.append(f"waybox exit status {proc.returncode}")

    def text(lp):
        return open(lp).read()

    def pids(lp, name):
        return [int(p) for p in re.findall(rf"wb-startup start {name} (\d+)", text(lp))]

    proc, log, lp = launch("main")
    kept = None
    try:
        time.sleep(1.8)
        t = text(lp)
        # 1. At startup.
        if not pids(lp, "sleeper") or not alive(pids(lp, "sleeper")[-1]):
            failures.append("an enabled program wasn't started")
        if pids(lp, "later"):
            failures.append("a disabled program was started")
        if "wb-startup exit once status 0" not in t or "restart once" in t:
            failures.append("a program that exits cleanly was restarted (or never ran)")
        if "wb-startup restart crashy in 1000 ms" not in t or len(pids(lp, "crashy")) < 2:
            failures.append("a crashed program wasn't started again after 1 s")
        if "wb-startup exit quitter status 4" not in t or "restart quitter" in t:
            failures.append('restart="no" was restarted')
        sleeper = pids(lp, "sleeper")[-1] if pids(lp, "sleeper") else None

        def pid_in(path):
            try:
                return int(open(path).read().strip())
            except (OSError, ValueError):
                return None
        leftover, kept = pid_in(left_pid), pid_in(kept_pid)
        stubborn = pid_in(stubborn_pid)
        brief = pids(lp, "brief")[-1] if pids(lp, "brief") else None
        if leftover is None or not alive(leftover) or kept is None or not alive(kept):
            failures.append("a program's background process didn't start (or ended)")

        # 2. Reconfigure with a changed list.
        write_rc([
            ("later", "exec sleep 1001", ""),
            ("once", f"echo ran &gt;&gt; {once_log}", ""),
            ("crashy", "exec sleep 1002", ""),
            ("keeper", f"sleep 1004 &amp; echo $! &gt; {kept_pid}", ""),
            ("brief", "sleep 2 &amp;", ""),  # kept: reaped by the supervisor's own look
        ])
        proc.send_signal(signal.SIGUSR2)
        time.sleep(1.0)
        t = text(lp)
        if "wb-startup stop sleeper" not in t:
            failures.append("a removed program wasn't stopped")
        elif sleeper and alive(sleeper):
            failures.append("a removed program is still running")
        if not pids(lp, "later") or not alive(pids(lp, "later")[-1]):
            failures.append("a program enabled on Reconfigure wasn't started")
        with open(once_log) as f:
            runs = f.read().count("ran")
        if runs != 1:
            failures.append(f"an unchanged one-shot program ran {runs} times")
        if leftover is not None and alive(leftover):
            failures.append("what a removed program left running wasn't stopped")
            os.kill(leftover, signal.SIGKILL)
        if kept is not None and not alive(kept):
            failures.append("an unchanged program's leftover process was stopped")
        crashy = pids(lp, "crashy")
        if not crashy or not alive(crashy[-1]):
            failures.append("a program whose command changed wasn't restarted with it")
        # The stubborn one: killed 3 s after the stop.
        time.sleep(3.0)
        if stubborn is None:
            failures.append("the stubborn program's background process didn't start")
        elif alive(stubborn):
            failures.append("what a removed program left running, ignoring SIGTERM, "
                            "wasn't killed")
            os.kill(stubborn, signal.SIGKILL)

        # 3. Killed: started again.
        later = pids(lp, "later")[-1] if pids(lp, "later") else None
        if later:
            os.kill(later, signal.SIGKILL)
            time.sleep(1.8)
            again = pids(lp, "later")
            if len(again) < 2 or not alive(again[-1]):
                failures.append("a killed program wasn't started again")
        # Brief's leftover ended long ago: its leader reaped, no zombie.
        if brief is None:
            failures.append("brief wasn't started")
        else:
            for _ in range(60):  # looked at every 5 s
                if not process_state(brief):
                    break
                time.sleep(0.1)
        if brief is not None and process_state(brief):
            failures.append(f"a leader kept for what it left running wasn't reaped "
                            f"after that ended: state {process_state(brief)}")
    finally:
        stop(proc, log)

    # 4. Exiting ends them.
    time.sleep(0.5)
    left = [p for name in ("later", "crashy") for p in pids(lp, name)[-1:] if alive(p)]
    if kept is not None and alive(kept):
        left.append(kept)
    if left:
        failures.append(f"programs still running after waybox exited: {left}")
        for p in left:
            os.kill(p, signal.SIGKILL)

    # 5. No <startup>: nothing.
    write_rc(None)
    proc, log, lp2 = launch("none")
    time.sleep(1.0)
    stop(proc, log)
    if "wb-startup" in text(lp2):
        failures.append("programs were started without a <startup> list")

    contents = text(lp) + text(lp2)
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration startup programs: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
