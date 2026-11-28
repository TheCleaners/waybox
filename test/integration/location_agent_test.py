#!/usr/bin/env python3
"""Integration test: waybox-location-agent, GeoClue's agent.

Private buses: a "system" one with a stand-in GeoClue (geoclue_standin.py),
a session one with the real xdg-permission-store; a headless Waybox for
the card's display. Each card is answered as a file says
(WAYBOX_LOCATION_TEST_ANSWER=@file), its wording logged.

1. GeoClue lists only geoclue-demo-agent: the agent's own ID is refused,
   and it registers as the demo agent.
2. Maps asks (a city): its card ("Maps wants to know where you are",
   "to within a city"); allowed this time: answered yes, nothing kept.
3. Maps asks again: a card again; denied always: answered no, kept in
   the permission store's location table ("NONE").
4. Maps asks again: answered no at once, no card.
5. Weather asks (exactly): allowed for the session; asking again for a
   street, answered yes at once (the session's answer covers less).
6. An app with no desktop file: the card names it as GeoClue said it.
   (Each card draws its map: the source tree's data, a fixed time zone.)
7. GeoClue stopping while a card waits: the card says so, and the next
   ask (GeoClue back) gets a card of its own.
8. GeoClue listing the agent's own ID: it registers as itself.
9. --policy=allow --note-kept: an app with nothing remembered is allowed
   at once, no card; a remembered denial still denies; each answer noted
   in a notification ("Allowed APP's location", why).
10. --ask-with-notifications: asked through the daemon (Deny, Allow);
    Allow chosen: allowed; dismissed: denied.
11. The daemon without actions: the card instead.

Skips (exit 77) without dbus-daemon, xdg-permission-store, the Python
GObject bindings, or a headless display.

Usage: location_agent_test.py <waybox> <waybox-location-agent>
"""
import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import wbtest

SKIP = 77
HERE = os.path.dirname(os.path.abspath(__file__))
STORE = "/usr/libexec/xdg-permission-store"


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    waybox, agent_bin = sys.argv[1:]
    try:
        import gi
        gi.require_version("Gio", "2.0")
        from gi.repository import Gio, GLib
    except (ImportError, ValueError):
        print("SKIP: no Python GObject bindings", file=sys.stderr)
        sys.exit(SKIP)
    if shutil.which("dbus-daemon") is None or not os.access(STORE, os.X_OK):
        print("SKIP: dbus-daemon or xdg-permission-store missing", file=sys.stderr)
        sys.exit(SKIP)
    workdir = tempfile.mkdtemp(prefix="waybox-locagent-", dir="/tmp")
    failures = []
    procs = []

    def bus():
        p = subprocess.Popen(["dbus-daemon", "--session", "--nofork", "--print-address"],
                             stdout=subprocess.PIPE, text=True)
        procs.append(p)
        return p.stdout.readline().strip()

    system_addr, session_addr = bus(), bus()
    data = os.path.join(workdir, "data")
    apps = os.path.join(data, "applications")
    os.makedirs(apps)
    for app_id, name in (("org.example.Maps", "Maps"), ("org.example.Weather", "Weather")):
        with open(os.path.join(apps, app_id + ".desktop"), "w") as f:
            f.write(f"[Desktop Entry]\nType=Application\nName={name}\nExec=true\n"
                    "Icon=find-location\n")
    answer_file = os.path.join(workdir, "answer")
    base = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=system_addr,
                DBUS_SESSION_BUS_ADDRESS=session_addr, XDG_DATA_HOME=data)
    for k in ("WAYLAND_DISPLAY", "DISPLAY", "WAYBOX_SOCK", "WAYBOX_PID"):
        base.pop(k, None)
    procs.append(subprocess.Popen([STORE], env=base, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL))

    # A headless Waybox: the card's display.
    runtime = os.path.join(workdir, "run")
    os.makedirs(runtime, 0o700)
    wenv = dict(base, WLR_BACKENDS="headless", WLR_RENDERER="pixman", XDG_RUNTIME_DIR=runtime,
                XDG_CONFIG_HOME=os.path.join(workdir, "config"), WB_RC_XML="", WB_MENU_XML="",
                ASAN_OPTIONS="detect_leaks=0")
    log = open(os.path.join(workdir, "waybox.log"), "w")
    wb = subprocess.Popen([waybox], env=wenv, stdout=log, stderr=subprocess.STDOUT)
    wbtest.reap_on_exit(wb)
    procs.append(wb)
    sock = None
    for _ in range(50):
        time.sleep(0.1)
        socks = [n for n in os.listdir(runtime)
                 if n.startswith("wayland-") and not n.endswith(".lock")]
        if socks:
            sock = socks[0]
            break
    if sock is None:
        for p in procs:
            p.kill()
        print("SKIP: headless backend did not bring up a display", file=sys.stderr)
        sys.exit(SKIP)
    aenv = dict(wenv, WAYLAND_DISPLAY=sock, GDK_BACKEND="wayland", GSK_RENDERER="cairo",
                GSETTINGS_BACKEND="memory", WAYBOX_LOCATION_TEST_ANSWER="@" + answer_file,
                WBCONF_GEO_DIR=os.path.join(HERE, "..", "..", "data", "geo"), TZ="Europe/Lisbon")

    def stand_in(allow):
        p = subprocess.Popen([sys.executable, os.path.join(HERE, "geoclue_standin.py")],
                             env=dict(base, GEOCLUE_ALLOW=allow), stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, text=True)
        procs.append(p)
        lines = queue.Queue()
        threading.Thread(target=lambda: [lines.put(l.strip()) for l in p.stdout],
                         daemon=True).start()
        return p, lines

    def wait_line(lines, prefix, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = lines.get(timeout=0.2)
            except queue.Empty:
                continue
            if line.startswith(prefix):
                return line
        return None

    def notifier(with_actions):
        p = subprocess.Popen([sys.executable, os.path.join(HERE, "notifications_standin.py")],
                             env=dict(base, NOTIFY_ACTIONS="1" if with_actions else "0"),
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        procs.append(p)
        lines = queue.Queue()
        threading.Thread(target=lambda: [lines.put(l.strip()) for l in p.stdout],
                         daemon=True).start()
        return p, lines

    def start_agent(*args):
        errlog = open(os.path.join(workdir, f"agent-{len(procs)}.log"), "w+")
        a = subprocess.Popen([agent_bin, *args], env=aenv, stdout=subprocess.DEVNULL,
                             stderr=errlog)
        procs.append(a)
        return a, errlog

    def agent_text(errlog):
        errlog.flush()
        errlog.seek(0)
        return errlog.read()

    def wait_text(errlog, needle, timeout=8.0):
        end = time.time() + timeout
        while needle not in agent_text(errlog) and time.time() < end:
            time.sleep(0.1)
        return needle in agent_text(errlog)

    def store():
        conn = Gio.DBusConnection.new_for_address_sync(
            session_addr, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
            Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)
        try:
            r = conn.call_sync("org.freedesktop.impl.portal.PermissionStore",
                               "/org/freedesktop/impl/portal/PermissionStore",
                               "org.freedesktop.impl.portal.PermissionStore", "Lookup",
                               GLib.Variant("(ss)", ("location", "location")), None,
                               Gio.DBusCallFlags.NONE, 3000, None)
            return dict(r.unpack()[0])
        except GLib.Error:
            return {}

    def ask(sp, lines, app, level, answer_with):
        with open(answer_file, "w") as f:
            f.write(answer_with)
        sp.stdin.write(f"ask {app} {level}\n")
        sp.stdin.flush()
        return wait_line(lines, f"answer {app} ") or wait_line(lines, f"error {app}", 0.5)

    try:
        # 1. Own ID refused: the demo agent's.
        sp, lines = stand_in("geoclue-demo-agent")
        wait_line(lines, "ready")
        agent, errlog = start_agent()
        if not wait_line(lines, "refused org.waybox.LocationAgent"):
            failures.append("the agent didn't try its own ID first")
        if not wait_line(lines, "agent geoclue-demo-agent "):
            failures.append("refused its own ID, it didn't register as the demo agent")
        # 2. Allowed this time.
        got = ask(sp, lines, "org.example.Maps", 4, "allow:once")
        if got != "answer org.example.Maps true 4":
            failures.append(f"allowed this time: {got}")
        if "card: Maps wants to know where you are|If you allow it, it learns where you are "\
                "to within a city." not in agent_text(errlog):
            failures.append("the card's wording: " + agent_text(errlog)[-400:])
        if "org.example.Maps" in store():
            failures.append("an answer for this time only was kept")
        # 3. Denied always.
        got = ask(sp, lines, "org.example.Maps", 4, "deny:always")
        if got != "answer org.example.Maps false 0":
            failures.append(f"denied always: {got}")
        if agent_text(errlog).count("card: Maps") != 2:
            failures.append("allowed this time only, Maps wasn't asked again")
        kept = store().get("org.example.Maps")
        if not kept or kept[0] != "NONE":
            failures.append(f"denied always, not kept: {kept}")
        # 4. Kept: no card.
        got = ask(sp, lines, "org.example.Maps", 4, "allow:once")
        if got != "answer org.example.Maps false 0" or \
                agent_text(errlog).count("card: Maps") != 2:
            failures.append(f"a kept denial still asked (or answered {got})")
        # 5. The session's answer.
        got = ask(sp, lines, "org.example.Weather", 8, "allow:session")
        if got != "answer org.example.Weather true 8":
            failures.append(f"allowed for the session: {got}")
        got = ask(sp, lines, "org.example.Weather", 6, "deny:once")
        if got != "answer org.example.Weather true 6" or \
                agent_text(errlog).count("card: Weather") != 1:
            failures.append(f"the session's answer didn't hold: {got}")
        # 6. No desktop file.
        got = ask(sp, lines, "some.unknown.tool", 5, "deny:once")
        if got != "answer some.unknown.tool false 0" or \
                "card: some.unknown.tool wants to know where you are|" not in agent_text(errlog) \
                or "to within a neighborhood" not in agent_text(errlog):
            failures.append(f"an app with no desktop file: {got}")
        # 7. GeoClue stops while a card waits.
        with open(answer_file, "w") as f:
            f.write("wait")
        # (an app with nothing kept, so a card shows)
        sp.stdin.write("ask some.unknown.tool 4\n")
        sp.stdin.flush()
        end = time.time() + 8
        while agent_text(errlog).count("card: some.unknown.tool") < 2 and time.time() < end:
            time.sleep(0.1)
        sp.kill()
        sp.wait(timeout=5)
        end = time.time() + 8
        while "card: closed, GeoClue stopped|some.unknown.tool no longer waits" not in agent_text(errlog) \
                and time.time() < end:
            time.sleep(0.1)
        if "card: closed, GeoClue stopped|some.unknown.tool no longer waits" not in agent_text(errlog):
            failures.append("GeoClue stopped mid-ask: the card didn't say so")
        sp, lines = stand_in("geoclue-demo-agent")
        wait_line(lines, "ready")
        if not wait_line(lines, "agent geoclue-demo-agent "):
            failures.append("GeoClue back: the agent didn't register again")
        got = ask(sp, lines, "another.unknown.tool", 4, "deny:once")
        if got != "answer another.unknown.tool false 0":
            failures.append(f"GeoClue back, a new ask: {got}")
        if agent.poll() is not None:
            failures.append("the agent exited: " + agent_text(errlog)[-600:])
        agent.terminate()
        agent.wait(timeout=10)
        sp.kill()
        # 8. Its own ID listed: itself.
        sp, lines = stand_in("org.waybox.LocationAgent,geoclue-demo-agent")
        wait_line(lines, "ready")
        agent, errlog = start_agent()
        if not wait_line(lines, "agent org.waybox.LocationAgent "):
            failures.append("with its ID listed, it didn't register as itself")
        agent.terminate()
        agent.wait(timeout=10)

        # 9. Allow every app with nothing remembered; each answer noted.
        np_, nlines = notifier(True)
        wait_line(nlines, "ready")
        agent, errlog = start_agent("--policy=allow", "--note-kept")
        wait_line(lines, "agent org.waybox.LocationAgent ")
        got = ask(sp, lines, "third.unknown.tool", 6, "deny:once")
        if got != "answer third.unknown.tool true 6" or "card: third" in agent_text(errlog):
            failures.append(f"--policy=allow: {got}")
        if not wait_line(nlines, "notify 1 Allowed third.unknown.tool's location|") or \
                not wait_text(errlog, "note: Allowed third.unknown.tool's location|"
                              "Location settings: allow every app"):
            failures.append("--note-kept: no note for the policy's answer: " +
                            agent_text(errlog)[-300:])
        got = ask(sp, lines, "org.example.Maps", 4, "allow:once")
        if got != "answer org.example.Maps false 0":
            failures.append(f"--policy=allow overrode a remembered denial: {got}")
        if not wait_line(nlines, "notify 2 Denied Maps's location|Remembered always") or \
                not wait_text(errlog, "note: Denied Maps's location|Remembered always"):
            failures.append("--note-kept: no note for the remembered denial")
        agent.terminate()
        agent.wait(timeout=10)

        # 10. Asked through notifications.
        agent, errlog = start_agent("--ask-with-notifications")
        wait_line(lines, "agent org.waybox.LocationAgent ")
        sp.stdin.write("ask fourth.unknown.tool 5\n")
        sp.stdin.flush()
        line = wait_line(nlines, "notify ")
        if not line or "fourth.unknown.tool wants to know where you are|" not in line \
                or not line.endswith("|deny,Deny,allow,Allow"):
            failures.append(f"asked by notification: {line}")
        else:
            np_.stdin.write(f"invoke {line.split()[1]} allow\n")
            np_.stdin.flush()
            got = wait_line(lines, "answer fourth.unknown.tool ")
            if got != "answer fourth.unknown.tool true 5":
                failures.append(f"Allow in the notification: {got}")
        sp.stdin.write("ask fourth.unknown.tool 5\n")
        sp.stdin.flush()
        line = wait_line(nlines, "notify ")
        if line:
            np_.stdin.write(f"dismiss {line.split()[1]}\n")
            np_.stdin.flush()
        got = wait_line(lines, "answer fourth.unknown.tool ")
        if got != "answer fourth.unknown.tool false 0":
            failures.append(f"the notification dismissed: {got} (asked: {line})")
        if "card: fourth" in agent_text(errlog):
            failures.append("asking through notifications, a card showed too")

        # 11. A daemon without actions: the card.
        np_.kill()
        np_.wait(timeout=5)
        np_, nlines = notifier(False)
        wait_line(nlines, "ready")
        got = ask(sp, lines, "fifth.unknown.tool", 5, "deny:once")
        if got != "answer fifth.unknown.tool false 0" or "card: fifth" not in agent_text(errlog):
            failures.append(f"no actions in notifications, no card: {got}")
    finally:
        for p in reversed(procs):
            if p.poll() is None:
                p.kill()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
        log.close()
    if failures:
        for f in failures:
            print(f"FAIL: {f}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("integration location_agent: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
