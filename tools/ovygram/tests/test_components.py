"""Lifecycle and confinement through the same manager and process host as the client."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
from zipfile import ZIP_DEFLATED, ZipFile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "Telegram/PluginSDK"))
from ovygram_runtime import Manager
from ovygram_package import MAX_PACKAGE, MAX_UNPACKED, InvalidPlugin, read_package

SOURCE = (ROOT / "tools/ovygram/examples/lifecycle.ovyplugin").read_bytes()
SOURCE = SOURCE[:SOURCE.index(b"class ")] + b"class Probe(BasePlugin):\n    pass\n"


@unittest.skipUnless(sys.platform == "win32" or shutil.which("bwrap"), "requires a supported sandbox")
class ComponentTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.manager = Manager(self.root / "profile")
        self.addCleanup(self.manager.shutdown)
        self.archive = self.root / "test.ovyzip"
        self.win = os.name == "nt"
        if self.win:
            probe = os.environ.get("OVYGRAM_COMPONENT_PROBE")
            if not probe:
                self.skipTest("needs the Windows component probe compiled by GitHub Actions")
            binary = sleeper = Path(probe)
        else:
            binary, sleeper = Path(shutil.which("grep")), Path(shutil.which("sleep"))
        from ovygram_runtime import plugin_metadata
        pid = plugin_metadata(SOURCE, "main.py")["id"]
        with ZipFile(self.archive, "w", ZIP_DEFLATED) as z:
            z.writestr("plugin.json", json.dumps({"id": pid, "entry": "main.py"}))
            z.writestr("main.py", SOURCE)
            z.write(binary, "bin/tool")
            z.write(sleeper, "bin/wait")
        self.owner = self.manager.install(str(self.archive))
        self.manager.enable(self.owner)
        self.assertIn(self.owner, self.manager._loaded, self.manager.errors)

    def command(self, op, **params):
        return self.manager.helpers.command(self.owner, op, params)

    def assert_component_files_removed(self):
        helpers = self.manager.helpers
        deadline = time.monotonic() + 12
        # Windows can release the executable mapping after the component exits.
        # Use the manager's normal retry path when removal was deferred.
        while helpers.pending_cleanup and time.monotonic() < deadline:
            helpers.poll()
            if helpers.pending_cleanup:
                time.sleep(0.1)
        self.assertFalse(helpers.pending_cleanup)
        self.assertFalse(list(self.manager.root.glob("helper-*")))

    def run_tool(self, arguments, config=""):
        if not self.win:
            self.command("start", resource="bin/tool", arguments=arguments, config=config)
            process = self.manager.helpers.children[self.owner][0]
            code = process.wait(timeout=12)
            self.assertFalse(self.command("status"))
            return code
        create = subprocess.Popen
        with tempfile.TemporaryFile() as errors:
            def capture_launcher(arguments, **kwargs):
                if any(Path(arg).name == "ovygram_helper_launcher.py" for arg in arguments):
                    kwargs["stderr"] = errors
                return create(arguments, **kwargs)

            with mock.patch("ovygram_helpers.subprocess.Popen", side_effect=capture_launcher):
                self.command("start", resource="bin/tool", arguments=arguments, config=config)
            process = self.manager.helpers.children[self.owner][0]
            code = process.wait(timeout=12)
            self.assertFalse(self.command("status"))
            errors.seek(0)
            self.assertEqual(code, 0, f"Windows process 0x{code:08X}: {errors.read(2048).decode(errors='replace')}")
            return code

    def test_stdio_private_file_and_cleanup(self):
        self.assertEqual(self.run_tool(["check-stdin"] if self.win else ["needle"], "needle\n"), 0)
        secret = self.root / "profile" / "private.txt"
        secret.write_text("needle\n", encoding="ascii")
        if self.win:
            identity = subprocess.check_output(["whoami"], text=True).strip()
            subprocess.run(["icacls", str(secret), "/inheritance:r", "/grant:r", identity + ":(F)"],
                           check=True, capture_output=True)
            self.assertEqual(self.run_tool(["check-private-file", str(secret)]), 0)
        else:
            self.assertNotEqual(self.run_tool(["needle", str(secret)]), 0)
        self.assert_component_files_removed()

    def test_disable_stops_component_and_withdraws_offer(self):
        args = ["wait"] if self.win else ["60"]
        self.command("start", resource="bin/wait", arguments=args, config="")
        process = self.manager.helpers.children[self.owner][0]
        with self.assertRaises(ValueError):
            self.command("start", resource="bin/wait", arguments=args, config="")
        with self.assertRaises(ValueError):
            self.command("offer", port=-1, user="foo", password="")
        if self.manager.host.status()["network"]:
            self.command("offer", port=12345, user="probe", password="test-only")
            self.assertIn(self.owner, self.manager.helpers.offers)
        self.manager.disable(self.owner)
        self.assertIsNotNone(process.poll())
        self.assertNotIn(self.owner, self.manager.helpers.offers)
        self.assert_component_files_removed()

    def test_only_verified_members_and_active_owners(self):
        for resource in ["../component", "C:/component", "missing"]:
            with self.subTest(resource=resource), self.assertRaises((KeyError, ValueError)):
                self.command("start", resource=resource, arguments=[], config="")
        original = self.manager.original(self.manager.records[self.owner]["current"])
        original.write_bytes(original.read_bytes() + b"x")
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.command("start", resource="bin/tool", arguments=[], config="")
        with self.assertRaises(ValueError):
            self.manager.helpers.command("another", "stop", {})
        self.assertFalse(self.manager.helpers.children)


class ProxyOfferTests(unittest.TestCase):
    def setUp(self):
        from ovygram_helpers import Helpers
        self.manager = mock.Mock()
        self.manager._loaded = {"instance"}
        self.manager.records = {"instance": {"plugin_id": "demo"}}
        self.manager.host.status.return_value = {"active": True, "network": True}
        self.helpers = Helpers(self.manager)
        process = mock.Mock()
        process.poll.return_value = None
        self.helpers.children["instance"] = process, None, mock.Mock()

    def offer(self, operation, **kwargs):
        return self.helpers.command("instance", operation, {
            "port": 15678, "user": "example", "password": "test-password", **kwargs})

    def test_independent_offers_and_expiration(self):
        self.offer("offer")
        old = self.helpers.offers["instance"]
        self.offer("offer_http", routes=["mini_apps"])
        browser = self.helpers.offers["instance:http"]
        self.assertEqual(browser["routes"], ["mini_apps"])
        self.assertEqual(browser["protocol"], "http")
        self.offer("offer", routes=["files"])
        self.assertNotEqual(self.helpers.offers["instance"]["token"], old["token"])
        self.assertEqual(self.helpers.offers["instance:http"], browser)
        self.helpers.stop("instance")
        self.assertFalse(self.helpers.offers)

    def test_wrong_scope_and_sandbox_are_rejected(self):
        for operation, scopes in [("offer", ["mini_apps"]), ("offer_http", ["api"]),
                                   ("offer_http", ["pages", "pages"]), ("offer", [])]:
            with self.subTest(operation=operation, scopes=scopes), self.assertRaises(ValueError):
                self.offer(operation, routes=scopes)
        self.manager.host.status.return_value = {"active": True, "network": False}
        with self.assertRaises(ValueError):
            self.offer("offer_http")
        self.assertFalse(self.helpers.offers)


class ArchiveLimitsTests(unittest.TestCase):
    def test_archive_and_unpacked_limits_are_independent(self):
        import io
        self.assertEqual((MAX_PACKAGE, MAX_UNPACKED), (32 << 20, 64 << 20))
        sample = hashlib.shake_256(b"archive size regression").digest(32768)
        with tempfile.TemporaryFile() as data:
            # Repeating a random block inside the deflate window stays under the ratio guard.
            block = sample[:16384] * 4 + sample[16384:] * 4
            with ZipFile(data, "w", ZIP_DEFLATED) as z:
                z.writestr("plugin.json", '{"id":"lifecycle","entry":"main.py"}')
                z.writestr("main.py", SOURCE)
                with z.open("bin/engine", "w") as out:
                    for _ in range(257):
                        out.write(block)
            data.seek(0)
            self.assertEqual(read_package(data.read(), "lifecycle.ovyzip")["id"], "lifecycle")
        buffer = io.BytesIO()
        with ZipFile(buffer, "w", ZIP_DEFLATED) as z:
            with z.open("too-large", "w") as out:
                for _ in range(65):
                    out.write(bytes(1 << 20))
        with self.assertRaisesRegex(InvalidPlugin, "64 MiB"):
            read_package(buffer.getvalue(), "lifecycle.ovyzip")
