#!/usr/bin/env python3
"""waybox-wallpaper chooses the image and the wallpaper program.

Stand-in swaybg/wpaperd record their arguments. Checks: a random image
from the wallpapers folders; one from a given folder; a given image (with
~ expanded); no image found: the colour alone; wpaperd when chosen; no
wallpaper program: an error.

Usage: wallpaper_test.py <data/wallpaper>
"""
import os
import shutil
import subprocess
import sys
import tempfile


def main():
    script = sys.argv[1]
    work = tempfile.mkdtemp(prefix="waybox-wallpaper-")
    failures = []
    try:
        bindir = os.path.join(work, "bin")
        os.makedirs(bindir)
        record = os.path.join(work, "args")
        for prog in ("swaybg", "wpaperd"):
            p = os.path.join(bindir, prog)
            with open(p, "w") as f:
                f.write(f'#!/bin/sh\necho "{prog} $*" > "{record}"\n')
            os.chmod(p, 0o755)
        # Only the stand-ins, and the basics the script uses.
        sysbin = os.path.join(work, "sysbin")
        os.makedirs(sysbin)
        for tool in ("sh", "find", "shuf", "tail", "printf", "sleep", "id"):
            path = shutil.which(tool)
            if path:
                os.symlink(path, os.path.join(sysbin, tool))
        share = os.path.join(work, "share")
        os.makedirs(os.path.join(share, "wallpapers"))
        open(os.path.join(share, "wallpapers", "dune.png"), "w").close()
        mine = os.path.join(work, "home", "Pictures", "Walls")
        os.makedirs(mine)
        open(os.path.join(mine, "sea.jpg"), "w").close()
        open(os.path.join(mine, "notes.txt"), "w").close()

        def run(args, env_extra=None, path=bindir + ":" + sysbin):
            if os.path.exists(record):
                os.remove(record)
            env = {"PATH": path, "HOME": os.path.join(work, "home"),
                   "XDG_DATA_DIRS": share, "XDG_DATA_HOME": os.path.join(work, "nodata")}
            env.update(env_extra or {})
            r = subprocess.run(["sh", script] + args, env=env, capture_output=True,
                               text=True, timeout=10)
            got = open(record).read().strip() if os.path.exists(record) else ""
            return r.returncode, got

        rc, got = run([])
        want = f"swaybg -c #303030 -m fill -i {share}/wallpapers/dune.png"
        if got != want:
            failures.append(f"default: {got!r} != {want!r}")
        rc, got = run(["-i", "~/Pictures/Walls", "-c", "#000000"])
        if got != f"swaybg -c #000000 -m fill -i {mine}/sea.jpg":
            failures.append(f"a folder (with ~): {got!r}")
        rc, got = run(["-i", os.path.join(mine, "sea.jpg"), "-m", "center"])
        if got != f"swaybg -c #303030 -m center -i {mine}/sea.jpg":
            failures.append(f"an image: {got!r}")
        rc, got = run([], {"XDG_DATA_DIRS": os.path.join(work, "empty")})
        if got != "swaybg -c #303030":
            failures.append(f"no image found: {got!r}")
        rc, got = run([], {"WAYBOX_WALLPAPER_BACKEND": "wpaperd"})
        if got != "wpaperd":
            failures.append(f"wpaperd: {got!r}")
        rc, got = run([], path=sysbin)
        if rc != 1 or got:
            failures.append(f"no wallpaper program: exit {rc}, {got!r}")
        rc, got = run(["-e", "soon"])
        if rc != 2:
            failures.append(f"a bad -e wasn't refused (exit {rc})")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    for f in failures:
        print(f"FAIL: {f}", file=sys.stderr)
    if not failures:
        print("wallpaper helper: OK")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
