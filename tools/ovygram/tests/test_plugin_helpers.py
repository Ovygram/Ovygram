from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

SDK = Path(__file__).resolve().parents[3] / "Telegram/PluginSDK"
sys.path.insert(0, str(SDK))
from ovygram_helpers import Helpers


class HelperTests(unittest.TestCase):
    def setUp(self):
        self.manager = SimpleNamespace(
            notify=Mock(), logger=Mock(), _loaded={"owner"},
            host=SimpleNamespace(status=lambda: {"active": True, "network": True}),
            records={"owner": {"plugin_id": "test"}},
        )
        self.helpers = Helpers(self.manager)

    def test_windows_mapping_lock_retries_cleanup_after_withdrawing_routes(self):
        locked = PermissionError("executable still mapped")
        locked.winerror = 5
        directory = Mock()
        directory.cleanup.side_effect = [locked, None]
        process = Mock()
        process.poll.return_value = 0
        self.helpers.children["owner"] = (process, 7, directory)
        self.helpers.offers["owner"] = {"port": 3000}
        self.helpers.offers["owner:http"] = {"port": 3001}
        with patch("ovygram_sandbox_windows.terminate_helper_job") as terminate, \
                patch("ovygram_helpers.release") as release, \
                patch("ovygram_helpers.time.monotonic", side_effect=[1, 3]):
            self.helpers.stop("owner")
            self.assertEqual(self.helpers.offers, {})
            self.assertEqual(self.helpers.children, {})
            self.assertEqual(len(self.helpers.pending_cleanup), 1)
            self.assertEqual(self.manager.notify.call_args.args[0]["method"], "proxy.refresh")
            terminate.assert_called_once_with(7)
            release.assert_called_once_with(7)
            process.kill.assert_not_called()
            process.wait.assert_called_once()
            self.helpers.poll()
        self.assertEqual(self.helpers.pending_cleanup, [])
        self.assertEqual(directory.cleanup.call_count, 2)

    def test_offer_validates_and_removes_the_indicator_label_with_the_route(self):
        process = Mock()
        process.poll.return_value = None
        self.helpers.children["owner"] = (process, None, Mock())
        settings = {"port": 3000, "user": "user", "password": "password", "routes": ["api"]}
        with self.assertRaisesRegex(ValueError, "indicator label"):
            self.helpers.command("owner", "offer", {**settings, "label": "unsafe\nname"})
        self.helpers.command("owner", "offer", {**settings, "label": "Proxy Tunnel"})
        self.assertEqual(self.helpers.offers["owner"]["label"], "Proxy Tunnel")
        self.helpers.stop("owner")
        self.assertEqual(self.helpers.offers, {})
