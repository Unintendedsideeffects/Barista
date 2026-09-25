#!/usr/bin/env python3
"""Only runs during a GamePad session; X11 and Chromium have no radio privileges."""
import os
import signal
import subprocess
import time
from pathlib import Path

children = []
stopping = False
# The bridge creates this after its screen timeout plus a further idle period;
# Chromium is then stopped (Xvfb and the bridge stay, so a touch can wake it)
# and relaunched as soon as the bridge removes the file.
PARK = Path("/tmp/gamepad-park")

def stop(_signum, _frame):
    global stopping
    stopping = True

def spawn(args):
    p = subprocess.Popen(args)
    children.append(p)
    return p

def main():
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    profile = Path.home() / "profile"
    profile.mkdir(parents=True, exist_ok=True)
    # These locks are machine-local; this profile belongs exclusively to this container.
    for name in ("SingletonLock", "SingletonCookie", "SingletonSocket"):
        (profile / name).unlink(missing_ok=True)
    chromium = None
    try:
        spawn(["Xvfb", ":99", "-screen", "0", "864x480x24", "-nolisten", "tcp"])
        for _ in range(50):
            if stopping:
                return
            if subprocess.run(["xdpyinfo"], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0:
                break
            time.sleep(0.1)
        else:
            raise RuntimeError("X11 did not become ready")
        spawn(["openbox"])
        browser = ["chromium", "--no-sandbox", "--disable-dev-shm-usage",
               "--disable-background-networking", "--disable-sync", "--disable-component-update",
               "--disable-gpu", "--no-first-run", "--no-default-browser-check",
               "--password-store=basic", "--user-data-dir=" + str(profile),
               "--window-size=864,480", "--window-position=0,0", "--kiosk",
               # Page console errors (e.g. a failed module load) reach the container log.
               "--enable-logging=stderr", "--log-level=0",
               "--app=" + os.environ["GAMEPAD_URL"]]
        if os.environ.get("GAMEPAD_DEBUG") == "1":
            browser += ["--remote-debugging-port=9222", "--remote-debugging-address=127.0.0.1"]
        PARK.unlink(missing_ok=True)
        chromium = subprocess.Popen(browser)
        spawn(["barista-dashboard-bridge"])
        while not stopping:
            if any(p.poll() is not None for p in children):
                raise RuntimeError("A dashboard process exited")
            parked = PARK.exists()
            if parked and chromium is not None:
                print("gamepad-dashboard: parking Chromium", flush=True)
                chromium.terminate()
                try:
                    chromium.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    chromium.kill()
                    chromium.wait()
                chromium = None
            elif not parked and chromium is None:
                print("gamepad-dashboard: relaunching Chromium", flush=True)
                chromium = subprocess.Popen(browser)
            elif chromium is not None and chromium.poll() is not None:
                raise RuntimeError("Chromium exited")
            time.sleep(0.5)
    finally:
        if chromium is not None and chromium.poll() is None:
            chromium.terminate()
            try:
                chromium.wait(timeout=8)
            except subprocess.TimeoutExpired:
                chromium.kill()
        for p in reversed(children):
            if p.poll() is None:
                p.terminate()
        deadline = time.monotonic() + 8
        for p in reversed(children):
            try:
                p.wait(timeout=max(0.1, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()

if __name__ == "__main__":
    main()
