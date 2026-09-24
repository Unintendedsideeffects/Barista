#!/usr/bin/env python3
"""Own the GamePad containers. The radio survives standby; the browser does not."""
import argparse
import json
import logging
import os
from pathlib import Path
import re
import selectors
import signal
import socket
import subprocess
import time

LOG = logging.getLogger("gamepad")
DEFAULT_ROOT = Path("/opt/barista/examples/on-demand")
CONTROL = Path("/run/barista-gamepad/control.sock")

class DisconnectGrace:
    def __init__(self, seconds):
        self.seconds = seconds
        self.disconnected_at = None

    def want_dashboard(self, connected, already_running, now):
        if connected:
            self.disconnected_at = None
            return True
        if not already_running:
            self.disconnected_at = None
            return False
        if self.disconnected_at is None:
            self.disconnected_at = now
        return now - self.disconnected_at < self.seconds

def engine_status(path):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(1)
        client.connect(str(path))
        client.sendall(b"status\n")
        data = b""
        while len(data) < 16384:
            part = client.recv(4096)
            if not part:
                break
            data += part
    lines = data.decode("utf-8", errors="replace").splitlines()
    if not lines or not lines[0].startswith("OK "):
        raise RuntimeError("Invalid engine status")
    return dict(line.split("=", 1) for line in lines[1:] if "=" in line)

class Runtime:
    def __init__(self, root, interface):
        self.root = root
        self.interface = interface

    def adapter_present(self):
        return (Path("/sys/class/net") / self.interface / "phy80211").exists()

    def paired(self):
        path = self.root / "state/radio/credentials.conf"
        if not path.is_file():
            return False
        entries = path.read_text().splitlines()
        return any(re.fullmatch(r"gamepad_mac=(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", x)
                   for x in entries)

    def running(self, service):
        result = subprocess.run(
            ["docker", "inspect", "-f", "{{.State.Running}}", "barista-gamepad-" + service],
            capture_output=True, text=True, timeout=10)
        return result.returncode == 0 and result.stdout.strip() == "true"

    def compose(self, *args, pair_code=""):
        env = dict(os.environ, GAMEPAD_PAIR_CODE=pair_code, GAMEPAD_INTERFACE=self.interface)
        result = subprocess.run(
            ["docker", "compose", "--profile", "on-demand", *args],
            cwd=self.root, env=env, text=True, capture_output=True, timeout=120)
        if result.returncode:
            # Keep diagnostics bounded; runtime status can contain local paths.
            raise RuntimeError(result.stderr.strip()[-1000:] or "Container operation failed")

    def start(self, service, code=""):
        self.compose("up", "-d", "--no-build", service, pair_code=code)

    def stop(self, service):
        self.compose("stop", service)

    def status(self):
        return engine_status(self.root / "run/engine.sock")

class Manager:
    def __init__(self, root, config, runtime=None, clock=time.monotonic):
        self.root = root
        self.config = config
        self.runtime = runtime or Runtime(root, config["interface"])
        self.clock = clock
        self.grace = DisconnectGrace(config["disconnect_grace_seconds"])
        self.radio = False
        self.dashboard = False
        self.pair_code = ""
        self.pair_deadline = 0
        self.retry_at = 0
        self.last_audit = -30
        self.status_missing_since = None
        self.error = ""
        self.state = {"state": "starting", "connected": False, "dashboard_running": False}
        self.paused = (root / "state/paused").exists()

    def stop_dashboard(self):
        if self.dashboard:
            self.runtime.stop("dashboard")
            self.dashboard = False
        self.grace.disconnected_at = None

    def stop_all(self):
        self.stop_dashboard()
        if self.radio:
            self.runtime.stop("radio")
            self.radio = False

    def publish(self, state, connected=False, **extra):
        self.state = {"state": state, "connected": connected,
                      "dashboard_running": self.dashboard,
                      "radio_running": self.radio, "updated_at": int(time.time()),
                      "error": self.error, **extra}
        path = self.root / "run/status.json"
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.state, indent=2) + "\n")
        tmp.chmod(0o644)
        tmp.replace(path)

    def tick(self):
        now = self.clock()
        try:
            # Reconcile containers after restart/crash, without forking docker every tick.
            if now - self.last_audit >= 15:
                self.radio = self.runtime.running("radio")
                self.dashboard = self.runtime.running("dashboard")
                self.last_audit = now
            if self.paused:
                self.stop_all()
                self.publish("paused")
                return
            if not self.runtime.adapter_present():
                self.stop_all()
                self.pair_code = ""
                self.publish("waiting_adapter")
                return
            paired = self.runtime.paired()
            if self.pair_code and now >= self.pair_deadline:
                self.stop_all()
                self.pair_code = ""
                self.error = "Pairing timed out; use gamepadctl pair to try again"
            if not paired and not self.pair_code:
                self.stop_all()
                self.publish("needs_pairing")
                return
            if now < self.retry_at:
                self.publish("retry_wait")
                return
            if not self.radio:
                self.runtime.start("radio", self.pair_code)
                self.radio = True
                self.status_missing_since = None
                self.error = ""
            try:
                engine = self.runtime.status()
            except (OSError, RuntimeError):
                if self.status_missing_since is None:
                    self.status_missing_since = now
                if not self.grace.want_dashboard(False, self.dashboard, now):
                    self.stop_dashboard()
                if now - self.status_missing_since >= 90:
                    raise RuntimeError("Radio engine stopped answering status requests")
                self.publish("radio_starting")
                return
            self.status_missing_since = None
            if engine.get("backend_last_error", "-") not in ("", "-"):
                raise RuntimeError("Radio: " + engine["backend_last_error"])
            if engine.get("phase") == "pairing":
                self.stop_dashboard()
                # Only expose the code after the actual AP entered pairing.
                self.publish("pairing_ready", pairing_symbols=engine.get("pair_symbols", ""))
                return
            if engine.get("phase") != "runtime":
                self.publish("radio_starting")
                return
            if self.pair_code and paired:
                self.pair_code = ""
            connected = engine.get("connected") == "1"
            if self.grace.want_dashboard(connected, self.dashboard, now):
                if not self.dashboard:
                    self.runtime.start("dashboard")
                    self.dashboard = True
            else:
                self.stop_dashboard()
            extra = {}
            if engine.get("battery_available") == "1":
                extra["battery"] = engine.get("battery")
            self.publish("active" if connected else
                         ("disconnect_grace" if self.dashboard else "standby"),
                         connected, **extra)
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            self.error = str(exc)
            LOG.error("%s", self.error)
            # A failed radio must not leave a browser consuming resources.
            try:
                self.stop_all()
            except (OSError, RuntimeError, subprocess.SubprocessError):
                LOG.exception("Could not stop containers")
            self.retry_at = now + self.config["retry_seconds"]
            self.publish("error")

    def command(self, request):
        command = request.get("command")
        if command == "status":
            return self.state
        if command == "pair":
            if self.paused:
                raise ValueError("Resume the manager before pairing")
            code = request.get("code", "")
            if not isinstance(code, str) or not re.fullmatch("[0-3]{4}", code):
                raise ValueError("Pairing code must be four digits from 0 to 3")
            if not self.runtime.adapter_present():
                raise ValueError("The dedicated GamePad adapter is not available yet")
            if self.state.get("connected"):
                raise ValueError("Disconnect the GamePad before starting a new pairing")
            self.stop_all()
            self.pair_code = code
            self.pair_deadline = self.clock() + self.config["pairing_timeout_seconds"]
            self.retry_at = 0
            self.error = ""
            return {"state": "pairing_requested", "message": "Wait for pairing_ready before SYNC"}
        if command in ("pause", "resume"):
            self.paused = command == "pause"
            marker = self.root / "state/paused"
            if self.paused:
                marker.touch()
                self.stop_all()
                self.pair_code = ""
            else:
                marker.unlink(missing_ok=True)
                self.retry_at = 0
                self.error = ""
            return {"state": "paused" if self.paused else "resuming"}
        if command == "cancel-pair":
            self.stop_all()
            self.pair_code = ""
            self.error = ""
            return {"state": "pairing_cancelled"}
        raise ValueError("Unknown command")

def serve(root, config):
    manager = Manager(root, config)
    CONTROL.parent.mkdir(parents=True, exist_ok=True)
    CONTROL.unlink(missing_ok=True)
    stop = False
    def shutdown(_sig, _frame):
        nonlocal stop
        stop = True
    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        server.bind(str(CONTROL))
        CONTROL.chmod(0o600)
        server.listen(4)
        server.setblocking(False)
        with selectors.DefaultSelector() as selector:
            selector.register(server, selectors.EVENT_READ)
            next_tick = 0
            try:
                while not stop:
                    now = time.monotonic()
                    if now >= next_tick:
                        manager.tick()
                        next_tick = time.monotonic() + (1 if manager.radio else 10)
                    for _key, _mask in selector.select(timeout=min(1, max(0, next_tick-time.monotonic()))):
                        client, _ = server.accept()
                        with client:
                            client.settimeout(2)
                            try:
                                data = b""
                                while b"\n" not in data and len(data) < 2048:
                                    part = client.recv(2048)
                                    if not part:
                                        break
                                    data += part
                                result = manager.command(json.loads(data))
                            except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as exc:
                                result = {"error": str(exc)}
                            try:
                                client.sendall((json.dumps(result) + "\n").encode())
                            except OSError:
                                pass
                        next_tick = 0
            finally:
                manager.stop_all()
                manager.publish("stopped")
                CONTROL.unlink(missing_ok=True)

def cli():
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["status", "pair", "cancel-pair", "pause", "resume"])
    parser.add_argument("code", nargs="?")
    args = parser.parse_args()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(65)
        try:
            client.connect(str(CONTROL))
        except FileNotFoundError:
            parser.error("GamePad manager is not ready; check barista-gamepad-manager.service")
        client.sendall((json.dumps({"command": args.command, "code": args.code}) + "\n").encode())
        result = b""
        while b"\n" not in result:
            data = client.recv(4096)
            if not data:
                break
            result += data
    response = json.loads(result)
    print(json.dumps(response, indent=2, ensure_ascii=False))
    raise SystemExit(1 if response.get("error") else 0)

if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    if Path(os.sys.argv[0]).name == "gamepadctl":
        cli()
    else:
        config_path = DEFAULT_ROOT / "manager.json"
        serve(DEFAULT_ROOT, json.loads(config_path.read_text()))
