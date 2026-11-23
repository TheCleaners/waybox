#!/usr/bin/env python3
"""Record the README tour GIF (and matching stills) on the headless backend.

Runs a private waybox instance (WLR_BACKENDS=headless, pixman), drives it with
wlrctl/wtype, captures frames with grim, captions them, and encodes a small
GIF (shared palette, no dithering, gifsicle -O3). Nothing touches your
session: config, runtime dir and HOME all live in a temporary directory, and
the shell prompt is a neutral "waybox$ ".

Usage:
  tools/record-tour.py [--build build] [--theme Onyx-Citrus] [--out screenshots]
                       [--no-stills] [--keep-frames DIR]

Writes <out>/tour.gif and, unless --no-stills, <out>/desktop.png, menu.png and
alttab.png. Requires: grim, wtype, wlrctl, swaybg, waybar, foot, dash,
magick (ImageMagick 7), gifsicle, and a waybox build (wbconf optional).

The menu hover positions assume the default menu.xml and a theme with a
~12px menu font (true of the built-in default and Onyx-Citrus); check the
contact sheet (--keep-frames) after switching to a very different theme.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H = 1280, 720          # headless output size
GIF_W, GIF_H = 960, 540   # GIF frame size
FPS = 10
FONT = "DejaVu-Sans-Bold"

TOOLS = ["grim", "wtype", "wlrctl", "swaybg", "waybar", "foot", "dash",
         "magick", "gifsicle"]


def theme_accent(theme):
    """Pick an accent colour from an Openbox theme's selected-menu-item colour."""
    if not theme:
        return None
    for base in [os.path.expanduser("~/.local/share/themes"),
                 os.path.expanduser("~/.themes"), "/usr/local/share/themes",
                 "/usr/share/themes"]:
        rc = os.path.join(base, theme, "openbox-3", "themerc")
        if os.path.exists(rc):
            text = open(rc).read()
            for key in ("menu.items.active.bg.colorTo", "menu.items.active.bg.color"):
                m = re.search(rf"^{re.escape(key)}:\s*(#[0-9a-fA-F]{{6}})", text, re.M)
                if m:
                    return m.group(1)
    return None


def write_config(root, theme, accent, wallpaper_color):
    cfg = os.path.join(root, "cfg")
    for d in ("waybox", "waybar", "foot"):
        os.makedirs(os.path.join(cfg, d), exist_ok=True)

    rc = open(os.path.join(REPO, "data", "rc.xml")).read()
    head = rc.index(">", rc.index("<openbox_config")) + 1
    extra = ""
    if theme:
        extra += f"\n  <theme><name>{theme}</name></theme>"
    rc = rc[:head] + extra + rc[head:]
    rules = """  <applications>
    <application title="Waybox Configuration*">
      <position><x>380</x><y>90</y></position>
    </application>
  </applications>
</openbox_config>"""
    tail = rc.rindex("</openbox_config>")
    rc = rc[:tail] + rules + "\n"
    open(os.path.join(cfg, "waybox", "rc.xml"), "w").write(rc)
    shutil.copyfile(os.path.join(REPO, "data", "menu.xml"),
                    os.path.join(cfg, "waybox", "menu.xml"))

    open(os.path.join(cfg, "foot", "foot.ini"), "w").write(
        "[main]\nfont=monospace:size=10\nshell=dash\n"
        "initial-window-size-pixels=600x380\n")
    json.dump({"layer": "top", "position": "top", "height": 26,
               "modules-left": ["wlr/taskbar"], "modules-right": ["clock"],
               "wlr/taskbar": {"format": "{icon} {title:.30}", "icon-size": 16},
               "clock": {"format": "{:%a %d %b  %H:%M}"}},
              open(os.path.join(cfg, "waybar", "config"), "w"))
    open(os.path.join(cfg, "waybar", "style.css"), "w").write(f"""
* {{ font-family: sans-serif; font-size: 12px; border: none; border-radius: 0; min-height: 0; }}
window#waybar {{ background: #181818; color: #e8e8e8; }}
#taskbar button {{ padding: 0 8px; color: #e8e8e8; }}
#taskbar button.active {{ background: {accent}; color: #ffffff; }}
#clock {{ padding: 0 12px; }}
""")
    wall = os.path.join(root, "wall.png")
    subprocess.run(["magick", "-size", f"{W}x{H}", f"xc:{wallpaper_color}", wall],
                   check=True)
    return cfg, wall


class Session:
    def __init__(self, root, cfg, waybox):
        self.root = root
        self.env = dict(os.environ, XDG_RUNTIME_DIR=root, XDG_CONFIG_HOME=cfg,
                        HOME=os.path.join(root, "home"), PS1="waybox$ ",
                        TERMINAL="foot --title=terminal",
                        WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                        WLR_HEADLESS_OUTPUTS="1", GSK_RENDERER="cairo")
        self.env.pop("WAYLAND_DISPLAY", None)
        os.makedirs(self.env["HOME"], exist_ok=True)
        self.log = open(os.path.join(root, "waybox.log"), "w")
        self.proc = subprocess.Popen([waybox], env=self.env, stdout=self.log,
                                     stderr=subprocess.STDOUT)
        for _ in range(50):
            time.sleep(0.1)
            socks = [n for n in os.listdir(root)
                     if n.startswith("wayland-") and not n.endswith(".lock")]
            if socks:
                self.env["WAYLAND_DISPLAY"] = socks[0]
                break
        else:
            sys.exit("waybox did not create a Wayland socket; see "
                     + os.path.join(root, "waybox.log"))
        self.children = []
        self.pos = [0, 0]
        self.caption = ""
        self.frames = []
        self._stop = False

    def run(self, *args):
        subprocess.run(list(args), env=self.env, capture_output=True)

    def spawn(self, *args):
        p = subprocess.Popen(list(args), env=self.env, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        self.children.append(p)
        return p

    # input helpers
    def glide(self, x, y, steps=12):
        sx, sy = self.pos
        for k in range(1, steps + 1):
            tx, ty = sx + round((x - sx) * k / steps), sy + round((y - sy) * k / steps)
            self.run("wlrctl", "pointer", "move", str(tx - self.pos[0]),
                     str(ty - self.pos[1]))
            self.pos = [tx, ty]
            time.sleep(0.03)

    def click(self, button="left"):
        self.run("wlrctl", "pointer", "click", button)

    def key(self, *chord):
        *mods, k = chord
        args = ["wtype"]
        for m in mods:
            args += ["-M", m]
        args += ["-k", k]
        for m in reversed(mods):
            args += ["-m", m]
        self.run(*args)

    def type(self, text):
        self.run("wtype", "-d", "35", text)

    def step(self, caption, hold=0.0):
        self.caption = caption
        time.sleep(hold)

    # capture
    def start_capture(self, frame_dir):
        def loop():
            i = 0
            while not self._stop:
                t = time.time()
                path = os.path.join(frame_dir, f"{i:05d}.png")
                subprocess.run(["grim", "-c", "-l", "0", path], env=self.env)
                self.frames.append((path, t, self.caption))
                i += 1
                time.sleep(max(0.0, 1.0 / FPS - (time.time() - t)))
        self.thread = threading.Thread(target=loop)
        self.thread.start()

    def still(self, path):
        subprocess.run(["grim", "-c", path], env=self.env, check=True)

    def close(self):
        self._stop = True
        if hasattr(self, "thread"):
            self.thread.join()
        for c in self.children:
            c.terminate()
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log.close()


def tour(s, wbconf, stills_dir):
    s.run("wlr-randr", "--output", "HEADLESS-1", "--custom-mode", f"{W}x{H}")
    s.spawn("swaybg", "-i", os.path.join(s.root, "wall.png"), "-m", "fill")
    s.spawn("waybar")
    time.sleep(1.5)
    s.run("wlrctl", "pointer", "move", "-9000", "-9000")
    s.glide(900, 500, 4)

    s.step("Waybox: a *box-style Wayland compositor", 2.0)
    s.step("Right-click the desktop for the root menu", 0.3)
    s.glide(420, 300)
    time.sleep(0.3)
    s.click("right")
    time.sleep(0.9)
    s.glide(470, 456, 10)        # hover "Waybox" -> submenu opens
    time.sleep(1.2)
    if stills_dir:
        s.still(os.path.join(stills_dir, "menu.png"))
    s.glide(470, 324, 10)        # back up to "Terminal"
    time.sleep(0.5)
    s.step("Launch a terminal from the menu", 0.2)
    s.click()
    time.sleep(1.6)
    s.step("Super+Return opens another; Smart placement avoids overlap", 0.3)
    s.key("logo", "Return")
    time.sleep(1.8)
    s.step("Titlebars and borders are drawn by Waybox", 0.3)
    s.type("wlr-randr\n")
    time.sleep(1.8)
    s.step("Super+Shift+M maximizes and restores", 0.2)
    s.key("logo", "shift", "m")
    time.sleep(1.3)
    s.key("logo", "shift", "m")
    time.sleep(1.0)
    s.step("Super+S shades a window to its titlebar", 0.2)
    s.key("logo", "s")
    time.sleep(1.4)
    s.key("logo", "shift", "s")
    time.sleep(1.0)
    s.step("Super+T toggles decorations per window", 0.2)
    s.key("logo", "t")
    time.sleep(1.3)
    s.key("logo", "t")
    time.sleep(1.0)
    s.step("Alt+Tab: the window switcher", 0.3)
    holder = subprocess.Popen(["wtype", "-M", "alt", "-k", "Tab", "-s", "900",
                               "-k", "Tab", "-s", "900", "-m", "alt"], env=s.env)
    time.sleep(0.6)
    if stills_dir:
        s.still(os.path.join(stills_dir, "alttab.png"))
    holder.wait()
    time.sleep(1.2)
    s.step("Output management: wlr-randr sets 1.25x scale live", 0.3)
    s.type("clear; wlr-randr --output HEADLESS-1 --scale 1.25\n")
    time.sleep(2.0)
    s.step("Fractional scaling: text stays sharp at 1.25x", 2.0)
    s.type("clear; wlr-randr --output HEADLESS-1 --scale 1\n")
    time.sleep(1.5)
    if wbconf:
        s.step("wbconf: settings with a live theme preview", 0.2)
        s.spawn(wbconf)
        time.sleep(3.2)
    if stills_dir:
        s.still(os.path.join(stills_dir, "desktop.png"))
    s.step("github.com/TheCleaners/waybox", 2.5)


def encode(frames, out_gif, accent, work):
    frames = [f for f in frames if f[2]]  # drop pre-roll frames with no caption
    gf = os.path.join(work, "gif")
    os.makedirs(gf, exist_ok=True)

    def captioned(i, out):
        path, _, caption = frames[i]
        subprocess.run(["magick", path, "-resize", f"{GIF_W}x{GIF_H}",
                        "-fill", "#181818", "-draw", f"rectangle 0,{GIF_H - 40} {GIF_W},{GIF_H}",
                        "-fill", accent, "-draw", f"rectangle 0,{GIF_H - 40} {GIF_W},{GIF_H - 38}",
                        "-font", FONT, "-pointsize", "19", "-fill", "white",
                        "-gravity", "south", "-annotate", "+0+9", caption, out], check=True)

    # One shared palette from a spread of frames, no dithering: unchanged
    # pixels stay identical between frames, which is what makes -O3 effective.
    sample = list(range(0, len(frames), max(1, len(frames) // 24)))
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        list(ex.map(lambda i: captioned(i, os.path.join(gf, f"s{i:05d}.png")), sample))
    palette = os.path.join(work, "palette.png")
    subprocess.run(["magick"] + [os.path.join(gf, f"s{i:05d}.png") for i in sample]
                   + ["+append", "-dither", "None", "-colors", "255",
                      "-unique-colors", palette], check=True)

    def to_gif(i):
        png = os.path.join(gf, f"{i:05d}.png")
        captioned(i, png)
        subprocess.run(["magick", png, "-dither", "None", "-remap", palette,
                        os.path.join(gf, f"{i:05d}.gif")], check=True)
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        list(ex.map(to_gif, range(len(frames))))

    args = ["gifsicle", "--loopcount=forever"]
    for i in range(len(frames)):
        d = frames[i + 1][1] - frames[i][1] if i + 1 < len(frames) else 2.5
        args += ["-d", str(max(2, round(d * 100))), os.path.join(gf, f"{i:05d}.gif")]
    raw = os.path.join(work, "raw.gif")
    subprocess.run(args + ["-o", raw], check=True, stderr=subprocess.DEVNULL)
    subprocess.run(["gifsicle", "-O3", raw, "-o", out_gif], check=True,
                   stderr=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser(
        description="Record the README tour GIF and stills on the headless backend.")
    ap.add_argument("--build", default=os.path.join(REPO, "build"))
    ap.add_argument("--theme", default="Onyx-Citrus",
                    help="Openbox theme name ('' for the built-in default)")
    ap.add_argument("--out", default=os.path.join(REPO, "screenshots"))
    ap.add_argument("--wallpaper", default="#4a4a4a")
    ap.add_argument("--no-stills", action="store_true")
    ap.add_argument("--keep-frames", metavar="DIR",
                    help="keep raw frames and a contact sheet here")
    a = ap.parse_args()

    missing = [t for t in TOOLS if shutil.which(t) is None]
    if missing:
        sys.exit("missing tools: " + ", ".join(missing))
    waybox = os.path.join(a.build, "waybox", "waybox")
    if not os.access(waybox, os.X_OK):
        sys.exit(f"no waybox binary at {waybox}; build first (ninja -C {a.build})")
    wbconf = os.path.join(a.build, "wbconf", "wbconf")
    wbconf = wbconf if os.access(wbconf, os.X_OK) else None

    accent = theme_accent(a.theme) or "#3b4a5a"
    # A short path under /tmp: Wayland socket paths are length-limited.
    root = tempfile.mkdtemp(prefix="wbtour.", dir="/tmp")
    try:
        cfg, _ = write_config(root, a.theme, accent, a.wallpaper)
        frame_dir = a.keep_frames or os.path.join(root, "frames")
        os.makedirs(frame_dir, exist_ok=True)
        stills = None if a.no_stills else os.path.join(root, "stills")
        if stills:
            os.makedirs(stills)

        s = Session(root, cfg, waybox)
        try:
            s.start_capture(frame_dir)
            tour(s, wbconf, stills)
        finally:
            s.close()
        print(f"captured {len(s.frames)} frames")

        os.makedirs(a.out, exist_ok=True)
        gif = os.path.join(a.out, "tour.gif")
        encode(s.frames, gif, accent, root)
        print(f"wrote {gif} ({os.path.getsize(gif) // 1024} KB)")
        if stills:
            for name in ("desktop.png", "menu.png", "alttab.png"):
                src = os.path.join(stills, name)
                if os.path.exists(src):
                    dst = os.path.join(a.out, name)
                    subprocess.run(["magick", src, "-strip", "-define",
                                    "png:compression-level=9", dst], check=True)
                    print(f"wrote {dst}")
        if a.keep_frames:
            picks = s.frames[:: max(1, len(s.frames) // 12)][:12]
            subprocess.run(["magick", "montage"] + [p for p, _, _ in picks]
                           + ["-tile", "3x4", "-geometry", "640x360+4+4",
                              os.path.join(a.keep_frames, "sheet.png")], check=False)
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
