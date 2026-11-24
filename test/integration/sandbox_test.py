#!/usr/bin/env python3
"""Integration test for the sandbox policy (security-context-v1 and the
global filter).

sandbox_client.c plays a sandbox engine: it makes a security context that
listens on a socket. wayland-info connected through that socket is a
sandboxed client; connected normally, it is not.

1. A sandboxed client doesn't see privileged protocols (screen capture,
   virtual keyboard, layer shell, session lock, output management, ...),
   and still sees ordinary ones (wl_compositor, xdg_wm_base, ...).
2. An ordinary client sees all of them.
3. Neither sees xwayland_shell_v1, which is only for Xwayland.

Skips (exit 77) without wayland-info or a headless display.

Usage: sandbox_test.py <waybox-binary> <sandbox_client-binary>
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

SKIP = 77

PRIVILEGED = ["ext_image_copy_capture_manager_v1", "zwp_virtual_keyboard_manager_v1",
              "zwlr_virtual_pointer_manager_v1", "zwlr_layer_shell_v1",
              "ext_session_lock_manager_v1", "zwlr_output_manager_v1",
              "ext_data_control_manager_v1", "zwlr_foreign_toplevel_manager_v1",
              "wp_security_context_manager_v1"]
ORDINARY = ["wl_compositor", "wl_seat", "xdg_wm_base", "xdg_activation_v1"]


def policy_list():
    """The privileged protocols, as waybox/security.cpp lists them."""
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                       "waybox", "security.cpp")
    body = open(src).read().split("kPrivileged", 1)[1].split("};", 1)[0]
    return set(re.findall(r'"([a-z0-9_]+)"', body))


def interfaces(env):
    out = subprocess.run(["wayland-info"], env=env, capture_output=True,
                         text=True, timeout=10).stdout
    return set(re.findall(r"interface: '([a-z0-9_]+)'", out))


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, client_bin = sys.argv[1], sys.argv[2]
    if shutil.which("wayland-info") is None:
        print("SKIP: wayland-info not found", file=sys.stderr)
        sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-sandbox-", dir="/tmp")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, WB_RC_XML="/dev/null", WB_MENU_XML="",
               ASAN_OPTIONS="detect_leaks=1:exitcode=99")
    env.pop("WAYLAND_DISPLAY", None)
    env.pop("DISPLAY", None)

    log_path = os.path.join(workdir, "waybox.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([waybox, "--debug"], env=env, stdout=log,
                            stderr=subprocess.STDOUT)
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
        sys.stderr.write(open(log_path).read())
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        proc.kill()
        sys.exit(SKIP)

    cenv = dict(env, WAYLAND_DISPLAY=sock)
    failures = []
    engine = None

    def expect(cond, what):
        if not cond:
            failures.append(what)

    try:
        ordinary = interfaces(cenv)
        sandbox_sock = os.path.join(runtime, "sandboxed")
        engine = subprocess.Popen([client_bin, sandbox_sock], env=cenv,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True)
        ready = engine.stdout.readline().strip()
        if ready != "ready":
            raise RuntimeError(f"sandbox engine didn't start: {ready!r}")
        sandboxed = interfaces(dict(cenv, WAYLAND_DISPLAY=sandbox_sock))

        expect(bool(sandboxed), "a sandboxed client could not connect")
        for name in PRIVILEGED:
            expect(name in ordinary, f"{name} not offered to ordinary clients")
            expect(name not in sandboxed, f"{name} offered to a sandboxed client")
        for name in ORDINARY:
            expect(name in sandboxed, f"{name} not offered to a sandboxed client")
        # Nothing else is hidden: everything an ordinary client sees, except
        # what the policy lists as privileged, reaches a sandboxed one too.
        for name in sorted(ordinary - sandboxed - policy_list()):
            failures.append(f"{name} hidden from a sandboxed client")
        for label, seen in (("ordinary", ordinary), ("sandboxed", sandboxed)):
            expect("xwayland_shell_v1" not in seen,
                   f"xwayland_shell_v1 offered to the {label} client")
        if proc.poll() is not None:
            failures.append(f"waybox exited (status {proc.returncode})")
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        failures.append(str(e))
    finally:
        if engine is not None and engine.poll() is None:
            engine.stdin.close()
            try:
                engine.wait(timeout=5)
            except subprocess.TimeoutExpired:
                engine.kill()
        time.sleep(0.3)
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                failures.append("waybox did not exit on SIGTERM")
        if proc.returncode not in (0, None):
            failures.append(f"waybox exit status {proc.returncode}")
        log.close()

    contents = open(log_path).read()
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration sandbox: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
