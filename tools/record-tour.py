#!/usr/bin/env python3
"""Record the README tour GIF (and matching stills) on the headless backend.

Runs a private waybox instance (WLR_BACKENDS=headless, pixman), drives it with
wlrctl/wtype, captures frames with grim, adds captions and title cards, and
encodes a GIF (shared palette, no dithering, gifsicle -O3). Nothing touches
your session: config, runtime dir and HOME all live in a temporary
directory, the shell prompt is a neutral "waybox", and fastfetch shows no
user or host names.

Usage:
  tools/record-tour.py [--build build] [--theme Onyx-Citrus] [--out screenshots]
                       [--no-stills] [--keep-frames DIR]

Writes <out>/tour.gif and, unless --no-stills, <out>/desktop.png, menu.png,
alttab.png and wbconf-search.png. Requires: grim, wtype, wlrctl, swaybg, waybar, foot, dash,
fastfetch, magick (ImageMagick 7), gifsicle, and a waybox build (wbconf
optional). Nothing from the real system is shown: no process lists, and
only a made-up home directory. Fonts: Inter (captions, Waybar) and Fira Code (terminals), with
fallbacks.

The root menu hover positions assume the default menu.xml and a theme with
a ~12px menu font (true of the built-in default and Onyx-Citrus); check the
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
PANEL = 30                # Waybar height

TOOLS = ["grim", "wtype", "wlrctl", "swaybg", "waybar", "foot", "dash",
         "fastfetch", "magick", "gifsicle"]


def font_file(pattern, fallback):
    """A font file for ImageMagick, by fontconfig pattern."""
    try:
        out = subprocess.run(["fc-match", "-f", "%{file}", pattern],
                             capture_output=True, text=True, check=True).stdout
        if out and os.path.exists(out):
            return out
    except (OSError, subprocess.CalledProcessError):
        pass
    return fallback


CAPTION_FONT = font_file("Inter:semibold", "DejaVu-Sans-Bold")
TITLE_FONT = font_file("Inter:bold", "DejaVu-Sans-Bold")
BODY_FONT = font_file("Inter", "DejaVu-Sans")


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


def mix(a, b, t):
    """#rrggbb colour a blended toward b by t."""
    ca = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    cb = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{round(x + (y - x) * t):02x}" for x, y in zip(ca, cb))


def make_wallpaper(path, accent):
    """A dark, smooth four-corner blend, warmed by the theme's accent in the
    bottom right."""
    base = "#121620"
    subprocess.run([
        "magick", "-size", f"{W}x{H}", "xc:", "-sparse-color", "Shepards",
        f"0,0 #0e1117  {W},0 #1a2232  0,{H} #161c28  "
        f"{W},{H} {mix(base, accent, 0.38)}  {W * 3 // 5},{H // 2} #1b2130",
        "-blur", "0x8", path], check=True)


def write_config(root, theme, accent):
    cfg = os.path.join(root, "cfg")
    home = os.path.join(root, "home")
    for d in ("waybox", "waybar", "foot", "fastfetch"):
        os.makedirs(os.path.join(cfg, d), exist_ok=True)
    os.makedirs(home, exist_ok=True)

    rc = open(os.path.join(REPO, "data", "rc.xml")).read()
    head = rc.index(">", rc.index("<openbox_config")) + 1
    extra = ""
    if theme:
        extra += f"\n  <theme><name>{theme}</name></theme>"
    rc = rc[:head] + extra + rc[head:]
    rc = re.sub(r"<number>\d+</number>", "<number>3</number>", rc, count=1)
    rc = rc.replace("<!-- <names><name>web</name><name>code</name></names> -->",
                    "<names><name>main</name><name>web</name><name>media</name></names>")
    rules = """  <applications>
    <application title="Waybox Configuration*">
      <position><x>330</x><y>70</y></position>
    </application>
  </applications>
</openbox_config>"""
    tail = rc.rindex("</openbox_config>")
    rc = rc[:tail] + rules + "\n"
    open(os.path.join(cfg, "waybox", "rc.xml"), "w").write(rc)
    shutil.copyfile(os.path.join(REPO, "data", "menu.xml"),
                    os.path.join(cfg, "waybox", "menu.xml"))

    # Terminals: Fira Code, a soft dark palette, some padding.
    open(os.path.join(cfg, "foot", "foot.ini"), "w").write(f"""[main]
font=Fira Code:size=10,monospace:size=10
shell=dash
pad=10x8
initial-window-size-pixels=600x380
[colors-dark]
background=1a1e26
foreground=d8dee9
regular0=2e3440
regular1=e06c75
regular2=98c379
regular3=e5c07b
regular4=61afef
regular5=c678dd
regular6=56b6c2
regular7=d8dee9
bright0=4c566a
bright1=e88388
bright2=a7d38a
bright3=ecca8b
bright4=7cbcf2
bright5=d391e4
bright6=6cc4cf
bright7=eceff4
""")

    # fastfetch with nothing personal: no user, host, kernel or hardware.
    json.dump({
        "logo": {"type": "small"},
        "display": {"separator": "  ", "key": {"width": 10}},
        "modules": [{"type": "wm", "key": "WM"}, {"type": "os", "key": "OS"},
                    {"type": "terminal", "key": "Terminal"},
                    {"type": "shell", "key": "Shell"},
                    {"type": "terminalfont", "key": "Font"},
                    "break", "colors"]},
        open(os.path.join(cfg, "fastfetch", "config.jsonc"), "w"))
    # A made-up home to list (never the real one).
    for d in ("Documents", "Music", "Pictures", "Projects/waybox", "Videos",
              "wallpapers"):
        os.makedirs(os.path.join(home, d), exist_ok=True)
    for f in ("notes.md", "todo.txt", "screenshot.png", "theme.tar.gz"):
        open(os.path.join(home, f), "w").close()
    script = os.path.join(home, "backup.sh")
    open(script, "w").write("#!/bin/sh\n")
    os.chmod(script, 0o755)

    json.dump({
        "layer": "top", "position": "top", "height": PANEL,
        "margin": "4 8 0 8", "spacing": 6,
        "modules-left": ["ext/workspaces", "wlr/taskbar"],
        "modules-right": ["clock"],
        "ext/workspaces": {"format": "{name}", "sort-by-id": True,
                           "on-click": "activate"},
        "wlr/taskbar": {"format": "{icon} {title:.26}", "icon-size": 14},
        "clock": {"format": "{:%a %d %b  %H:%M}"}},
        open(os.path.join(cfg, "waybar", "config"), "w"))
    open(os.path.join(cfg, "waybar", "style.css"), "w").write(f"""
* {{ font-family: Inter, sans-serif; font-size: 12px; border: none;
     border-radius: 0; min-height: 0; }}
window#waybar {{ background: rgba(16, 19, 25, 0.88); color: #d8dee9;
                 border-radius: 9px; }}
#workspaces {{ margin-left: 4px; }}
#workspaces button {{ padding: 0 10px; margin: 4px 2px; color: #8b95a7;
                      border-radius: 6px; }}
#workspaces button.active {{ background: {accent}; color: #ffffff; }}
#taskbar button {{ padding: 0 8px; margin: 4px 2px; color: #d8dee9;
                   border-radius: 6px; }}
#taskbar button.active {{ background: rgba(255, 255, 255, 0.12); }}
#clock {{ padding: 0 12px; font-weight: 600; }}
""")
    wall = os.path.join(root, "wall.png")
    make_wallpaper(wall, accent)
    return cfg, home, wall


class Session:
    def __init__(self, root, cfg, home, waybox):
        self.root = root
        esc = "\033"
        self.env = dict(os.environ, XDG_RUNTIME_DIR=root, XDG_CONFIG_HOME=cfg,
                        HOME=home, XDG_CURRENT_DESKTOP="Waybox",
                        PS1=f"{esc}[1;38;2;97;175;239mwaybox{esc}[0m "
                            f"{esc}[38;2;139;149;167m~{esc}[0m $ ",
                        TERMINAL="foot --title=terminal",
                        WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                        WLR_HEADLESS_OUTPUTS="1", GSK_RENDERER="cairo",

                        # Dark GTK windows, whatever the recording desktop
                        # prefers (that comes over the session bus).
                        GTK_THEME="Adwaita:dark",
                        # wbconf's settings schema and integration files,
                        # from the source tree.
                        WBCONF_SCHEMA_DIR=os.path.join(REPO, "data", "wbconf"),
                        XDG_DATA_DIRS=os.path.join(root, "share") + ":" +
                            (os.environ.get("XDG_DATA_DIRS") or "/usr/local/share:/usr/share"))
        os.makedirs(os.path.join(root, "share", "waybox"), exist_ok=True)
        os.symlink(os.path.join(REPO, "data", "integrations"),
                   os.path.join(root, "share", "waybox", "integrations"))
        # Recording from inside a Waybox session: don't inherit its config
        # paths (they would win over ours) or its displays.
        for var in ("WAYLAND_DISPLAY", "DISPLAY", "WB_RC_XML", "WB_MENU_XML",
                    "WB_ICON_THEME"):
            self.env.pop(var, None)
        # A private session bus, not the user's: a GtkApplication such as
        # wbconf would hand off to the user's running copy instead of
        # opening here (and Waybar wants a bus).
        self.bus = subprocess.Popen(["dbus-daemon", "--session", "--nofork",
                                     "--print-address=1"], stdout=subprocess.PIPE,
                                    text=True)
        self.env["DBUS_SESSION_BUS_ADDRESS"] = self.bus.stdout.readline().strip()
        self.log = open(os.path.join(root, "waybox.log"), "w")
        # Started in the made-up home, so terminals open there.
        self.proc = subprocess.Popen([waybox, "--debug"], env=self.env,
                                     stdout=self.log, stderr=subprocess.STDOUT,
                                     cwd=home)
        for _ in range(50):
            time.sleep(0.1)
            socks = [n for n in os.listdir(root)
                     if n.startswith("wayland-") and not n.endswith(".lock")]
            if socks:
                self.env["WAYLAND_DISPLAY"] = socks[0]
                break
        else:
            # Don't leave it running (its runtime directory is about to go).
            self.bus.terminate()
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
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

    def logtext(self):
        self.log.flush()
        return open(os.path.join(self.root, "waybox.log")).read()

    def last_geom(self, tag):
        m = re.findall(rf"wb-geom {tag} (\d+)x(\d+)\+(-?\d+)\+(-?\d+)", self.logtext())
        return tuple(map(int, m[-1])) if m else None

    # input helpers
    def glide(self, x, y, steps=14):
        sx, sy = self.pos
        for k in range(1, steps + 1):
            # ease in and out
            t = k / steps
            t = t * t * (3 - 2 * t)
            tx, ty = sx + round((x - sx) * t), sy + round((y - sy) * t)
            self.run("wlrctl", "pointer", "move", str(tx - self.pos[0]),
                     str(ty - self.pos[1]))
            self.pos = [tx, ty]
            time.sleep(0.025)

    def click(self, button="left"):
        self.run("wlrctl", "pointer", "click", button)

    def key(self, *chord, pause=0.0):
        *mods, k = chord
        args = ["wtype"]
        for m in mods:
            args += ["-M", m]
        args += ["-k", k]
        for m in reversed(mods):
            args += ["-m", m]
        self.run(*args)
        time.sleep(pause)

    def type(self, text):
        self.run("wtype", "-d", "45", text)

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
        self.bus.terminate()
        self.log.close()


def tour(s, wbconf, stills_dir):
    s.run("wlr-randr", "--output", "HEADLESS-1", "--custom-mode", f"{W}x{H}")
    s.spawn("swaybg", "-i", os.path.join(s.root, "wall.png"), "-m", "fill")
    s.spawn("waybar")
    time.sleep(1.8)
    s.run("wlrctl", "pointer", "move", "-9000", "-9000")
    s.glide(900, 480, 4)

    # The root menu, and a terminal from it.
    s.step("Right-click the desktop for the root menu", 0.4)
    s.glide(420, 300)
    time.sleep(0.3)
    s.click("right")
    time.sleep(0.9)
    s.glide(470, 456, 12)        # hover "Waybox": its submenu opens
    time.sleep(1.2)
    if stills_dir:
        s.still(os.path.join(stills_dir, "menu.png"))
    s.glide(470, 324, 12)        # back up to "Terminal"
    time.sleep(0.5)
    s.click()
    time.sleep(1.4)
    s.step("Frames, menus and the switcher follow your Openbox theme", 0.3)
    s.type("clear; fastfetch\n")
    time.sleep(2.2)

    # A second terminal; tile both.
    s.step("Super+Return opens a terminal; Smart placement avoids overlap", 0.3)
    s.key("logo", "Return", pause=1.4)
    s.type("ls --color=always -p\n")
    time.sleep(1.6)
    s.step("Super+Left and Super+Right tile windows to each half", 0.3)
    s.key("logo", "Right", pause=0.9)
    s.run("wlrctl", "toplevel", "focus", "title:foot")
    time.sleep(0.4)
    s.key("logo", "Left", pause=1.6)

    # The window menu on the right-hand window: roll it up and down.
    s.step("Right-click a title bar for the window menu", 0.3)
    right = s.last_geom("tile-right") or (W // 2, H - PANEL, W // 2, PANEL + 24)
    rw, _, rx, ry = right
    s.glide(rx + rw // 2, ry - 8)
    time.sleep(0.3)
    s.click("right")
    time.sleep(0.8)
    for _ in range(6):           # Send to desktop ... Roll up
        s.key("Down", pause=0.25)
    time.sleep(0.4)
    s.step("Roll up: just the title bar stays", 0.1)
    s.key("Return", pause=1.8)
    s.key("logo", "shift", "s", pause=1.0)   # roll back down

    # Desktops.
    s.step("Ctrl+Alt+Right: another desktop; Waybar follows along", 0.3)
    s.key("ctrl", "alt", "Right", pause=1.2)
    s.key("logo", "Return", pause=1.2)
    s.type("echo a fresh desktop\n")
    time.sleep(1.0)
    s.key("ctrl", "alt", "Left", pause=1.4)

    # Alt+Tab.
    s.step("Alt+Tab: the window switcher", 0.3)
    holder = subprocess.Popen(["wtype", "-M", "alt", "-k", "Tab", "-s", "900",
                               "-k", "Tab", "-s", "900", "-m", "alt"], env=s.env)
    time.sleep(0.6)
    if stills_dir:
        s.still(os.path.join(stills_dir, "alttab.png"))
    holder.wait()
    time.sleep(1.2)

    if wbconf:
        s.step("wbconf: every setting in one list, with a live theme preview", 0.2)
        s.spawn(wbconf)
        for _ in range(40):
            if "org.waybox.wbconf" in s.logtext():  # mapped (wb-geom)
                break
            time.sleep(0.2)
        time.sleep(3.0)
    if stills_dir:
        s.still(os.path.join(stills_dir, "desktop.png"))
    if wbconf:
        s.step("Type to search: settings, and programs to start with the session", 0.3)
        s.type("night")
        time.sleep(2.4)
        if stills_dir:
            s.still(os.path.join(stills_dir, "wbconf-search.png"))
    s.step("", 0.2)


# ---- encoding ----

def caption_badge(text, accent, path):
    """A rounded, translucent caption pill with an accent dot."""
    subprocess.run([
        "magick", "-background", "none", "-fill", "white",
        "-font", CAPTION_FONT, "-pointsize", "18", f"label:{text}",
        "-bordercolor", "none", "-border", "22x10", path], check=True)
    w, h = map(int, subprocess.run(["magick", "identify", "-format", "%w %h", path],
                                   capture_output=True, text=True,
                                   check=True).stdout.split())
    w += 14  # room for the dot
    subprocess.run([
        "magick", "-size", f"{w}x{h}", "xc:none",
        "-fill", "rgba(14,17,23,0.86)", "-draw",
        f"roundrectangle 0,0 {w - 1},{h - 1} {h // 2},{h // 2}",
        "-fill", accent, "-draw", f"circle 20,{h // 2} 24,{h // 2}",
        path, "-geometry", "+16+0", "-composite", path], check=True)


def title_card(wall, accent, title, subtitle, path):
    subprocess.run([
        "magick", wall, "-resize", f"{GIF_W}x{GIF_H}!",
        "-fill", "rgba(10,12,16,0.35)", "-draw", f"rectangle 0,0 {GIF_W},{GIF_H}",
        "-gravity", "center",
        "-font", TITLE_FONT, "-pointsize", "64", "-fill", "white",
        "-annotate", "+0-34", title,
        "-fill", accent, "-draw", f"roundrectangle {GIF_W // 2 - 40},{GIF_H // 2 + 6} "
        f"{GIF_W // 2 + 40},{GIF_H // 2 + 10} 2,2",
        "-font", BODY_FONT, "-pointsize", "22", "-fill", "#c9d1de",
        "-annotate", "+0+42", subtitle,
        path], check=True)


def encode(frames, out_gif, accent, wall, work):
    gf = os.path.join(work, "gif")
    os.makedirs(gf, exist_ok=True)
    # Keep from the first captioned frame on.
    first = next((i for i, f in enumerate(frames) if f[2]), 0)
    frames = frames[first:]

    badges = {}
    for _, _, cap in frames:
        if cap and cap not in badges:
            badges[cap] = os.path.join(gf, f"badge{len(badges)}.png")
            caption_badge(cap, accent, badges[cap])

    def captioned(i, out):
        path, _, caption = frames[i]
        args = ["magick", path, "-resize", f"{GIF_W}x{GIF_H}"]
        if caption:
            args += [badges[caption], "-gravity", "south", "-geometry", "+0+18",
                     "-composite"]
        subprocess.run(args + [out], check=True)

    intro = os.path.join(gf, "intro.png")
    outro = os.path.join(gf, "outro.png")
    title_card(wall, accent, "Waybox", "An Openbox-style Wayland compositor", intro)
    title_card(wall, accent, "Waybox", "github.com/TheCleaners/waybox", outro)

    # One shared palette from a spread of frames and the cards, no
    # dithering: unchanged pixels stay identical between frames, which is
    # what makes -O3 effective.
    sample = list(range(0, len(frames), max(1, len(frames) // 24)))
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        list(ex.map(lambda i: captioned(i, os.path.join(gf, f"s{i:05d}.png")), sample))
    palette = os.path.join(work, "palette.png")
    subprocess.run(["magick"] + [os.path.join(gf, f"s{i:05d}.png") for i in sample]
                   + [intro, "+append", "-dither", "None", "-colors", "255",
                      "-unique-colors", palette], check=True)

    def remap(png, gif):
        subprocess.run(["magick", png, "-dither", "None", "-remap", palette, gif],
                       check=True)

    def to_gif(i):
        png = os.path.join(gf, f"{i:05d}.png")
        captioned(i, png)
        remap(png, os.path.join(gf, f"{i:05d}.gif"))
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        list(ex.map(to_gif, range(len(frames))))
    remap(intro, os.path.join(gf, "intro.gif"))
    remap(outro, os.path.join(gf, "outro.gif"))

    args = ["gifsicle", "--loopcount=forever", "-d", "250",
            os.path.join(gf, "intro.gif")]
    for i in range(len(frames)):
        d = frames[i + 1][1] - frames[i][1] if i + 1 < len(frames) else 0.5
        args += ["-d", str(max(2, round(d * 100))), os.path.join(gf, f"{i:05d}.gif")]
    args += ["-d", "300", os.path.join(gf, "outro.gif")]
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

    accent = theme_accent(a.theme) or "#4f7cba"
    # A short path under /tmp: Wayland socket paths are length-limited.
    root = tempfile.mkdtemp(prefix="wbtour.", dir="/tmp")
    try:
        cfg, home, wall = write_config(root, a.theme, accent)
        frame_dir = a.keep_frames or os.path.join(root, "frames")
        os.makedirs(frame_dir, exist_ok=True)
        stills = None if a.no_stills else os.path.join(root, "stills")
        if stills:
            os.makedirs(stills)

        s = Session(root, cfg, home, waybox)
        try:
            s.start_capture(frame_dir)
            tour(s, wbconf, stills)
        finally:
            s.close()
        print(f"captured {len(s.frames)} frames")

        os.makedirs(a.out, exist_ok=True)
        gif = os.path.join(a.out, "tour.gif")
        encode(s.frames, gif, accent, wall, root)
        print(f"wrote {gif} ({os.path.getsize(gif) // 1024} KB)")
        if stills:
            for name in ("desktop.png", "menu.png", "alttab.png", "wbconf-search.png"):
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
