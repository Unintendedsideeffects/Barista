import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("manager", Path(__file__).parents[1] / "runtime/manager.py")
mod = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(mod)

class FakeRuntime:
    def __init__(self):
        self.present = True
        self.credentials = True
        self.live = {"radio": False, "dashboard": False}
        self.calls = []
        self.reply = {"phase": "runtime", "connected": "0", "backend_last_error": "-"}
        self.unreachable = False
    def adapter_present(self): return self.present
    def paired(self): return self.credentials
    def running(self, service): return self.live[service]
    def start(self, service, code=""):
        self.calls.append(("start", service))
        self.live[service] = True
    def stop(self, service):
        self.calls.append(("stop", service))
        self.live[service] = False
    def status(self):
        if self.unreachable: raise OSError("not ready")
        return self.reply

class ManagerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "run").mkdir()
        (self.root / "state").mkdir()
        self.now = 100.
        self.rt = FakeRuntime()
        self.manager = mod.Manager(self.root, {
            "interface": "wlangamepad", "disconnect_grace_seconds": 45,
            "pairing_timeout_seconds": 180, "retry_seconds": 60
        }, self.rt, lambda: self.now)
    def tick(self, advance=0):
        self.now += advance
        self.manager.tick()
    def connect(self):
        self.rt.reply["connected"] = "1"
        self.tick()

    def test_missing_adapter_starts_nothing(self):
        self.rt.present = False
        self.tick()
        self.assertEqual(self.rt.calls, [])
        self.assertEqual(self.manager.state["state"], "waiting_adapter")

    def test_no_credentials_never_auto_pairs(self):
        self.rt.credentials = False
        self.tick()
        self.assertEqual(self.rt.calls, [])
        self.assertEqual(self.manager.state["state"], "needs_pairing")

    def test_idle_only_starts_radio_and_connection_starts_browser_once(self):
        self.tick()
        self.assertEqual(self.rt.calls, [("start", "radio")])
        self.assertEqual(self.manager.state["state"], "standby")
        self.connect()
        for _ in range(20): self.tick(1)
        self.assertEqual(self.rt.calls.count(("start", "dashboard")), 1)

    def test_disconnect_grace_and_reconnection_preserve_radio(self):
        self.connect()
        self.rt.reply["connected"] = "0"
        self.tick()
        self.tick(44)
        self.assertTrue(self.rt.live["dashboard"])
        self.connect()
        self.assertNotIn(("stop", "dashboard"), self.rt.calls)
        self.rt.reply["connected"] = "0"
        self.tick()
        self.tick(45)
        self.assertFalse(self.rt.live["dashboard"])
        self.assertTrue(self.rt.live["radio"])
        self.assertNotIn(("stop", "radio"), self.rt.calls)
        self.connect()
        self.assertEqual(self.rt.calls.count(("start", "dashboard")), 2)

    def test_hot_unplug_stops_both(self):
        self.connect()
        self.rt.present = False
        self.tick()
        self.assertEqual(self.rt.live, {"radio": False, "dashboard": False})
        self.assertEqual(self.manager.state["state"], "waiting_adapter")

    def test_restart_adopts_running_containers(self):
        self.rt.live = {"radio": True, "dashboard": True}
        self.rt.reply["connected"] = "1"
        self.tick()
        self.assertEqual(self.rt.calls, [])

    def test_pairing_symbols_only_after_radio_ready(self):
        self.rt.credentials = False
        self.manager.command({"command": "pair", "code": "0123"})
        self.rt.unreachable = True
        self.tick()
        self.assertNotIn("pairing_symbols", self.manager.state)
        self.rt.unreachable = False
        self.rt.reply = {"phase": "pairing", "pair_symbols": "♠ ♥ ♦ ♣"}
        self.tick()
        self.assertEqual(self.manager.state["state"], "pairing_ready")
        self.assertEqual(self.manager.state["pairing_symbols"], "♠ ♥ ♦ ♣")
        self.tick(180)
        self.assertEqual(self.manager.state["state"], "needs_pairing")
        self.assertFalse(self.rt.live["radio"])

    def test_invalid_pair_request_is_rejected(self):
        for code in ("0124", "012", "0000;touch /tmp/no", None):
            with self.assertRaises(ValueError):
                self.manager.command({"command": "pair", "code": code})
        self.assertEqual(self.rt.calls, [])

    def test_radio_failure_stops_browser_and_backs_off(self):
        self.connect()
        self.rt.reply["backend_last_error"] = "AP failed"
        self.tick()
        self.assertEqual(self.manager.state["state"], "error")
        self.assertFalse(self.rt.live["dashboard"])
        count = len(self.rt.calls)
        self.tick(30)
        self.assertEqual(len(self.rt.calls), count)
        self.assertEqual(self.manager.state["state"], "retry_wait")

    def test_hung_engine_releases_browser_then_restarts_radio(self):
        self.connect()
        self.rt.unreachable = True
        self.tick()
        self.tick(46)
        self.assertFalse(self.rt.live["dashboard"])
        self.tick(45)
        self.assertFalse(self.rt.live["radio"])
        self.assertEqual(self.manager.state["state"], "error")

    def test_pause_and_resume_are_persistent(self):
        self.connect()
        self.manager.command({"command": "pause"})
        self.assertTrue((self.root / "state/paused").exists())
        self.tick()
        self.assertEqual(self.manager.state["state"], "paused")
        self.manager.command({"command": "resume"})
        self.assertFalse((self.root / "state/paused").exists())
        self.tick()
        self.assertEqual(self.manager.state["state"], "active")

if __name__ == "__main__":
    unittest.main()
