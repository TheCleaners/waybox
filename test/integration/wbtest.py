"""Shared by the integration tests.

reap_on_exit(proc): the Waybox (or other program) a test started goes when
the test does, however it ends: normally, on an exception the test doesn't
catch, or when meson stops it (SIGTERM on a timeout). Otherwise it would
keep running, orphaned, after a failed or timed-out test.
"""
import atexit
import os
import signal
import subprocess
import sys

# Run by hand inside a Waybox session too, a test's clients mustn't reach
# that session's Waybox: its socket and pid go (each test sets its own
# Waybox's where it needs them).
os.environ.pop("WAYBOX_SOCK", None)
os.environ.pop("WAYBOX_PID", None)
# Nor the session's D-Bus (a test's config errors would notify the user's
# desktop): tests that need a bus start a private one.
os.environ["DBUS_SESSION_BUS_ADDRESS"] = "disabled:"

_procs = []


def _reap():
    for proc in _procs:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def _on_sigterm(signum, frame):
    sys.exit(128 + signum)  # unwinds: finally blocks, then atexit


def reap_on_exit(proc):
    if not _procs:
        atexit.register(_reap)
        if signal.getsignal(signal.SIGTERM) in (signal.SIG_DFL, None):
            signal.signal(signal.SIGTERM, _on_sigterm)
    _procs.append(proc)
    return proc
