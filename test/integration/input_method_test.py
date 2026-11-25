#!/usr/bin/env python3
"""Integration test for input methods (text-input-v3, input-method-v2).

ime_client.c plays the input method (fcitx5, ibus) and text_input_client.c
an application with a text field:

1. Only one input method at a time: a second one is told it's unavailable.
2. When the application's text field gets focus, the input method is
   activated and told its surrounding text and content type.
3. The input method's preedit and committed text reach the application.
4. With a keyboard grab, keys go to the input method and not to the
   application; keys it passes on through its virtual keyboard reach the
   application once (and don't loop back into the grab), and the grab
   never gets the input method's own keyboard (a keymap loop, seen with
   ibus).
5. Its candidate popup is shown just below the text cursor.
6. When the application closes, the input method is deactivated; Waybox
   keeps running when the input method exits.

Skips (exit 77) without wtype, grim or magick, or a headless display.

Usage: input_method_test.py <waybox> <ime_client> <text_input_client>
"""
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

SKIP = 77


class Client:
    """A test client printing one event per line."""

    def __init__(self, argv, env):
        self.proc = subprocess.Popen(argv, env=env, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True)
        self.lines = queue.Queue()
        self.seen = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.lines.put(line.rstrip("\n"))

    def send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()

    def wait_for(self, pattern, timeout=5.0):
        """The first new line matching pattern (a regex), or None."""
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = self.lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            self.seen.append(line)
            if re.search(pattern, line):
                return line
        return None

    def collect(self, seconds):
        """All new lines within the next few seconds."""
        got, end = [], time.time() + seconds
        while time.time() < end:
            try:
                got.append(self.lines.get(timeout=max(0.01, end - time.time())))
            except queue.Empty:
                break
        self.seen += got
        return got

    def stop(self):
        if self.proc.poll() is None:
            try:
                self.proc.stdin.close()
            except BrokenPipeError:
                pass
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    waybox, ime_bin, app_bin = sys.argv[1:]
    for tool in ("wtype", "grim", "magick"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not found", file=sys.stderr)
            sys.exit(SKIP)

    workdir = tempfile.mkdtemp(prefix="waybox-ime-", dir="/tmp")
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    env = dict(os.environ, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
               XDG_RUNTIME_DIR=runtime, WB_RC_XML="/dev/null", WB_MENU_XML="",
               ASAN_OPTIONS=("detect_leaks=1:" if sys.platform.startswith("linux") else "") + "exitcode=99")
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
    clients = []

    def expect(cond, what):
        if not cond:
            failures.append(what)

    try:
        ime = Client([ime_bin], cenv)
        clients.append(ime)
        if ime.wait_for("^ready$") is None:
            raise RuntimeError("the input method didn't start")

        # 2. Focus a text field.
        app = Client([app_bin], cenv)
        clients.append(app)
        expect(app.wait_for("^configured$") is not None, "the application didn't map")
        expect(app.wait_for("^ti-enter$") is not None,
               "the text input didn't enter the focused window")
        expect(ime.wait_for("^activate$") is not None, "the input method wasn't activated")
        expect(ime.wait_for("^surrounding hello 5 5$") is not None,
               "the input method wasn't told the surrounding text")
        expect(ime.wait_for("^content 0 13$") is not None,
               "the input method wasn't told the content type")

        # 3. Preedit and commit.
        ime.send("preedit にほ")
        expect(app.wait_for("^preedit にほ ") is not None, "the preedit didn't arrive")
        ime.send("commit 日本語")
        expect(app.wait_for("^commit 日本語$") is not None,
               "the committed text didn't arrive")
        ime.send("delete 2 0")
        expect(app.wait_for("^delete 2 0$") is not None,
               "the surrounding-text deletion didn't arrive")

        # 4. Keyboard grab.
        ime.send("grab")
        ime.wait_for("^grabbed$")
        # The grab must not hand the input method its own virtual keyboard:
        # giving that the grab's keymap would change the grab's keymap, again
        # and again.
        keymaps = [l for l in ime.collect(0.5) if l == "keymap"]
        expect(len(keymaps) <= 2, f"keymap feedback loop ({len(keymaps)} keymaps)")
        subprocess.run(["wtype", "a"], env=cenv, capture_output=True)
        expect(ime.wait_for(r"^gkey \d+ 1$") is not None,
               "the grabbing input method didn't get the key")
        keys = [l for l in app.collect(0.6) if l.startswith("key ")]
        expect(not keys, f"a grabbed key also reached the application: {keys}")

        ime.send("forward")
        ime.wait_for("^forwarding$")
        subprocess.run(["wtype", "b"], env=cenv, capture_output=True)
        # One press each: a passed-on key looping back into the grab would
        # show up as more presses at the input method.
        time.sleep(0.8)
        grabbed = [l for l in ime.collect(0.2) if re.match(r"^gkey \d+ 1$", l)]
        keys = [l for l in app.collect(0.2) if re.match(r"^key \d+ 1$", l)]
        expect(len(grabbed) == 1,
               f"the input method should get the key press once, got {grabbed}")
        expect(len(keys) == 1,
               f"a passed-on key should reach the application once, got {keys}")

        # 5. The candidate popup, below the cursor (50,60 2x20 in the window).
        ime.send("popup")
        rect = ime.wait_for(r"^rect ")
        expect(rect == "rect 0 -20 2 20",
               f"popup not placed just below the text cursor: {rect}")
        time.sleep(0.4)
        shot = os.path.join(workdir, "shot.png")
        subprocess.run(["grim", shot], env=cenv, capture_output=True)
        red = subprocess.run(
            ["magick", shot, "-fill", "black", "+opaque", "#ff0000", "-fill", "white",
             "-opaque", "#ff0000", "-format", "%[fx:int(mean*w*h+0.5)]", "info:"],
            capture_output=True, text=True).stdout.strip()
        expect(red == str(200 * 100), f"the popup isn't fully on screen ({red} red pixels)")

        # 1. A second input method (last: its virtual keyboard would become
        # the seat's keyboard, and the grab check above needs the first's).
        second = Client([ime_bin], cenv)
        clients.append(second)
        expect(second.wait_for("^unavailable$") is not None,
               "a second input method was not told it's unavailable")

        # 6. Close the application, then the input method.
        app.send("close")
        app.proc.wait(timeout=5)
        expect(ime.wait_for("^deactivate$") is not None,
               "the input method wasn't deactivated when the application closed")
        ime.stop()
        time.sleep(0.4)
        expect(proc.poll() is None, "waybox exited after the input method did")
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        failures.append(str(e))
    finally:
        for c in clients:
            c.stop()
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
        for c, name in zip(clients, ("ime", "app", "second")):
            print(f"--- {name}: {c.seen}", file=sys.stderr)
        sys.stderr.write(contents[-3000:])
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration input method: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
