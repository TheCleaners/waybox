#!/usr/bin/env python3
"""Integration test for colour management (color-management-v1,
color-representation-v1) and the <hdr> option.

On the Vulkan renderer (the GPU's render node):

1. Both protocols are advertised.
2. Windows still draw and screenshots still work.
3. <hdr outputs="HEADLESS-1"/> on an output without HDR support falls back
   to SDR (logged), and the output keeps working.

On the GLES2 renderer, colour management is not offered and the log says
it needs Vulkan.

Skips (exit 77) without wayland-info, grim, foot, magick, a GPU render
node, a working Vulkan renderer, or wlroots 0.20.

Usage: color_management_test.py <waybox-binary>
"""
import glob
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


class Waybox:
    def __init__(self, binary, workdir, tag, renderer, render_node):
        self.runtime = os.path.join(workdir, f"run-{tag}")
        os.makedirs(self.runtime, 0o700)
        cfg = os.path.join(workdir, f"cfg-{tag}")
        os.makedirs(os.path.join(cfg, "waybox"))
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <waybox><hdr outputs="HEADLESS-1"/></waybox>
</openbox_config>
""")
        self.env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER=renderer,
                        WLR_RENDER_DRM_DEVICE=render_node,
                        # No implicit Vulkan layers (overlays, capture tools):
                        # the test must not depend on what is installed.
                        VK_LOADER_LAYERS_DISABLE="~implicit~",
                        XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=self.runtime,
                        WB_RC_XML="", WB_MENU_XML="",
                        # Real GPU drivers (Mesa, the Vulkan ICDs the loader
                        # probes and unloads) leave allocations at exit that
                        # can't all be suppressed by name. Leak checking is
                        # off here; ASan still catches memory errors.
                        ASAN_OPTIONS="detect_leaks=0:exitcode=99")
        for var in ("WAYLAND_DISPLAY", "DISPLAY"):
            self.env.pop(var, None)
        self.log_path = os.path.join(workdir, f"waybox-{tag}.log")
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen([binary, "--debug"], env=self.env,
                                     stdout=self.log, stderr=subprocess.STDOUT)
        self.sock = None
        for _ in range(60):
            time.sleep(0.1)
            if self.proc.poll() is not None:
                break
            socks = [n for n in os.listdir(self.runtime)
                     if n.startswith("wayland-") and not n.endswith(".lock")]
            if socks:
                self.sock = socks[0]
                break
        self.cenv = dict(self.env, WAYLAND_DISPLAY=self.sock or "")

    def logtext(self):
        self.log.flush()
        return open(self.log_path).read()

    def globals(self):
        out = subprocess.run(["wayland-info"], env=self.cenv, capture_output=True,
                             text=True).stdout
        return out

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.log.close()
        return self.proc.returncode


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    waybox = sys.argv[1]
    for tool in ("wayland-info", "grim", "foot", "magick"):
        need(tool)
    nodes = sorted(glob.glob("/dev/dri/renderD*"))
    if not nodes or not os.access(nodes[0], os.R_OK | os.W_OK):
        print("SKIP: no usable GPU render node", file=sys.stderr)
        sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-color-", dir="/tmp")
    failures = []

    def expect(cond, what):
        if not cond:
            failures.append(what)

    vk = Waybox(waybox, workdir, "vulkan", "vulkan", nodes[0])
    try:
        if vk.sock is None or "Color management enabled" not in vk.logtext():
            log = vk.logtext()
            vk.stop()
            reason = ("wlroots < 0.20" if "Color management" not in log
                      else "no working Vulkan renderer")
            print(f"SKIP: {reason}", file=sys.stderr)
            sys.stderr.write(log[-1500:])
            sys.exit(SKIP)

        info = vk.globals()
        expect("wp_color_manager_v1" in info, "color-management-v1 not advertised")
        expect("wp_color_representation_manager_v1" in info,
               "color-representation-v1 not advertised")

        foot = subprocess.Popen(["foot"], env=vk.cenv, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        time.sleep(1.5)
        shot = os.path.join(workdir, "shot.png")
        r = subprocess.run(["grim", shot], env=vk.cenv, capture_output=True, text=True)
        expect(r.returncode == 0, f"screenshot failed: {r.stderr.strip()}")
        if r.returncode == 0:
            mean = float(subprocess.run(["magick", shot, "-format", "%[fx:mean]",
                                         "info:"], capture_output=True,
                                        text=True).stdout or 0)
            expect(mean > 0.01, "nothing drawn (a black screen)")
        foot.terminate()
        foot.wait()

        expect("does not support HDR10" in vk.logtext() or "HDR on" in vk.logtext(),
               "the <hdr> setting was not handled")
    finally:
        status = vk.stop()
        if status not in (0, None):
            failures.append(f"waybox (Vulkan) exit status {status}")

    gl = Waybox(waybox, workdir, "gles2", "gles2", nodes[0])
    try:
        if gl.sock is not None:
            expect("wp_color_manager_v1" not in gl.globals(),
                   "colour management offered on GLES2")
            expect("needs the Vulkan renderer" in gl.logtext(),
                   "no log line saying colour management needs Vulkan")
    finally:
        gl.stop()

    contents = open(vk.log_path).read() + open(gl.log_path).read()
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration colour management: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
