#!/usr/bin/env python3
"""Integration test for dialogs: xdg-foreign and xdg-dialog.

dialog_client plays two programs: one exports its window (xdg-foreign v2),
the other opens a dialog and attaches it to that window, as a desktop
portal's file chooser does.

1. The other program's dialog opens centred over the exported window.
2. With xdg-dialog's modal flag, clicking the parent window focuses the
   dialog instead.
3. A plain (non-modal) dialog: clicking the parent focuses the parent, and
   the dialog stays above it (it's still visible).
4. Modal, further: activating the parent from a taskbar while another
   window covers it brings up the parent too, not just the dialog; a
   titlebar double-click on the parent maximizes the parent, not the
   dialog; and with the parent fullscreen, the dialog is drawn above it.

Skips (exit 77) without wlrctl, grim, magick or foot, or a headless display.

Usage: dialogs_test.py <waybox-binary> <dialog_client>
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


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, client = sys.argv[1:]
    for tool in ("wlrctl", "grim", "magick", "foot"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-dialog-", dir="/tmp")
    failures = []
    logs = []

    def expect(cond, what):
        if not cond:
            failures.append(what)

    def session(mode):
        runtime = os.path.join(workdir, f"run-{mode}")
        os.makedirs(runtime, 0o700)
        cfg = os.path.join(workdir, f"cfg-{mode}")
        os.makedirs(os.path.join(cfg, "waybox"))
        with open(os.path.join(cfg, "waybox", "rc.xml"), "w") as f:
            f.write("""<?xml version="1.0" encoding="UTF-8"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <applications>
    <!-- The parent and the window that covers it (4a) in the same spot. -->
    <application class="parent"><position><x>0</x><y>0</y></position></application>
    <application class="cover"><position><x>0</x><y>0</y></position></application>
  </applications>
  <mouse><context name="Client">
    <mousebind button="Middle" action="Press"><action name="ToggleMaximize"/></mousebind>
  </context></mouse>
</openbox_config>
""")
        env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                   XDG_CONFIG_HOME=cfg, XDG_RUNTIME_DIR=runtime, WB_RC_XML="", WB_MENU_XML="",
                   ASAN_OPTIONS="detect_leaks=1:exitcode=99")
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("DISPLAY", None)
        log_path = os.path.join(workdir, f"waybox-{mode}.log")
        logs.append(log_path)
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
            print("SKIP: headless backend did not bring up a display", file=sys.stderr)
            proc.kill()
            sys.exit(SKIP)
        cenv = dict(env, WAYLAND_DISPLAY=sock)

        def logtext():
            log.flush()
            return open(log_path).read()

        def geometries():
            return [tuple(map(int, g)) for g in
                    re.findall(r"wb-geom mapped (\d+)x(\d+)\+(-?\d+)\+(-?\d+)", logtext())]

        procs = []
        try:
            parent = subprocess.Popen([client, "export", "parent"], env=cenv,
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      text=True)
            procs.append(parent)
            line = parent.stdout.readline().strip()
            for _ in range(20):
                if line.startswith("handle "):
                    break
                line = parent.stdout.readline().strip()
            if not line.startswith("handle "):
                raise RuntimeError(f"no handle from the exporting window: {line!r}")
            handle = line.split()[1]
            time.sleep(0.8)
            dialog = subprocess.Popen([client, "import", "dialog", handle, mode], env=cenv,
                                      stdin=subprocess.PIPE, stdout=subprocess.DEVNULL)
            procs.append(dialog)
            time.sleep(1.2)
            geos = geometries()
            if len(geos) < 2:
                raise RuntimeError(f"windows not mapped: {geos}")
            (pw, ph, px, py), (cw, ch, cx, cy) = geos[0], geos[-1]

            # 1. Centred over the parent.
            expect(abs((cx + cw / 2) - (px + pw / 2)) <= 2 and
                   abs((cy + ch / 2) - (py + ph / 2)) <= 2,
                   f"{mode}: dialog {cw}x{ch}+{cx}+{cy} not centred over the parent "
                   f"{pw}x{ph}+{px}+{py}")

            # 2/3. Click the parent away from the dialog.
            subprocess.run(["wlrctl", "pointer", "move", "-5000", "-5000"], env=cenv,
                           capture_output=True)
            subprocess.run(["wlrctl", "pointer", "move", str(px + 10), str(py + 10)],
                           env=cenv, capture_output=True)
            time.sleep(0.2)
            subprocess.run(["wlrctl", "pointer", "click", "left"], env=cenv,
                           capture_output=True)
            time.sleep(0.5)
            focus = re.findall(r"Keyboard focus is now on surface: (\S+)", logtext())
            if mode == "modal":
                expect(focus and focus[-1] == "dialog",
                       f"modal: clicking the parent focused {focus[-1:]} not the dialog")
                expect("wb-focus parent -> modal dialog" in logtext(),
                       "modal: focus wasn't redirected to the dialog")
            else:
                expect(focus and focus[-1] == "parent",
                       f"plain: clicking the parent focused {focus[-1:]}")
            def pixel_at(x, y):
                shot = os.path.join(workdir, "shot.png")
                subprocess.run(["grim", "-g", f"{x},{y} 1x1", shot], env=cenv,
                               capture_output=True)
                return subprocess.run(["magick", shot, "-format", "%[pixel:p{0,0}]",
                                       "info:"], capture_output=True, text=True).stdout.strip()

            pixel = pixel_at(cx + cw // 2, cy + ch // 2)
            expect(pixel == "srgb(255,0,0)",
                   f"{mode}: the dialog isn't above its parent after the click ({pixel})")

            def wlrctl(*args):
                return subprocess.run(["wlrctl", *args], env=cenv, capture_output=True)

            if mode == "modal":
                # 4a. Cover the parent with another window, then activate the
                # parent as a taskbar does: the parent comes up as well.
                # foot: a different colour from the (blue) parent.
                cover = subprocess.Popen(["foot", "-a", "cover"], env=cenv,
                                         stdout=subprocess.DEVNULL,
                                         stderr=subprocess.DEVNULL)
                procs.append(cover)
                time.sleep(1.0)
                wlrctl("toplevel", "focus", "app_id:parent")
                time.sleep(0.5)
                corner = pixel_at(px + 5, py + 5)
                expect(corner == "srgb(0,0,255)" and
                       pixel_at(cx + cw // 2, cy + ch // 2) == "srgb(255,0,0)",
                       f"modal: activating the parent left it under another window "
                       f"({corner})")
                cover.kill()
                time.sleep(0.4)

                # 4b. A binding on the parent (middle-click: maximize) acts on
                # the parent, not on its modal dialog.
                wlrctl("pointer", "move", "-5000", "-5000")
                wlrctl("pointer", "move", str(px + 10), str(py + 10))
                time.sleep(0.2)
                wlrctl("pointer", "click", "middle")
                time.sleep(0.6)
                maximized = re.findall(r"wb-geom maximize-on \S+ (\S+)", logtext())
                expect(maximized == ["parent"],
                       f"modal: the binding maximized {maximized}, not the parent")
                wlrctl("pointer", "click", "middle")  # and back
                time.sleep(0.4)

                # 4c. Parent fullscreen: the dialog is drawn above it.
                wlrctl("toplevel", "fullscreen", "app_id:parent")
                time.sleep(0.6)
                fs_pixel = pixel_at(cx + cw // 2, cy + ch // 2)  # the dialog stays put
                expect(fs_pixel == "srgb(255,0,0)",
                       f"modal: the dialog is hidden under its fullscreen parent ({fs_pixel})")
        except RuntimeError as e:
            failures.append(str(e))
        finally:
            for p in procs:
                if p.poll() is None:
                    p.kill()
            time.sleep(0.3)
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
            log.close()
            if proc.returncode not in (0, None):
                failures.append(f"{mode}: waybox exit status {proc.returncode}")

    session("modal")
    session("plain")

    contents = "".join(open(p).read() for p in logs)
    if any(s in contents for s in ("Sanitizer", "Assertion", "runtime error",
                                   "use-after-free")):
        failures.append("sanitizer/assert output")
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration dialogs: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
