import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[3]
EXAMPLE = ROOT / "tools/ovygram/examples/lifecycle.ovyplugin"
sys.path.insert(0, str(ROOT / "Telegram/PluginSDK"))
from ovygram_runtime import InProcessManager as Manager, read_package
from ovygram_package import InvalidPlugin


class PluginRuntimeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.profile = self.root / "profile"

    def test_proxy_route_selections_survive_restart_and_are_removed_with_owner(self):
        first = Manager(self.profile)
        owner = first.install(str(EXAMPLE))
        first.save_proxy_routes({"api": owner, "pages": owner})
        with self.assertRaises(ValueError):
            first.save_proxy_routes({"api": "another-installation"})
        first.shutdown()
        second = Manager(self.profile)
        self.addCleanup(second.shutdown)
        self.assertEqual(second.proxy_routes, {"api": owner, "pages": owner})
        second.remove(owner)
        self.assertEqual(second.proxy_routes, {})

    def test_missing_release_installs_as_false_in_every_local_format(self):
        source = (EXAMPLE.read_bytes().replace(b"\r\n", b"\n")
                  .replace(b"__release__ = False\n", b""))
        self.assertNotIn(b"__release__", source)
        for suffix in (".ovyplugin", ".py", ".plugin", ".ovyzip", ".zip"):
            with self.subTest(suffix=suffix):
                profile = self.root / ("profile" + suffix)
                path = self.root / ("lifecycle" + suffix)
                if suffix in (".ovyzip", ".zip"):
                    with zipfile.ZipFile(path, "w") as archive:
                        archive.writestr("plugin.json", '{"id":"lifecycle","entry":"main.py"}')
                        archive.writestr("main.py", source)
                else:
                    path.write_bytes(source)
                original = path.read_bytes()
                manager = Manager(profile)
                self.addCleanup(manager.shutdown)
                self.assertIs(manager.inspect(str(path))["metadata"]["release"], False)
                identity = manager.install(str(path))
                self.assertIs(manager.records[identity]["current"]["metadata"]["release"], False)
                self.assertEqual(Path(manager.share_file(identity)).read_bytes(), original)
                manager.enable(identity)
                self.assertIn(identity, manager.loaded)
                manager.shutdown()
                restored = Manager(profile)
                self.addCleanup(restored.shutdown)
                self.assertIs(restored.records[identity]["current"]["metadata"]["release"], False)
                restored.shutdown()

    def test_release_default_does_not_hide_invalid_or_duplicate_values(self):
        source = (EXAMPLE.read_bytes().replace(b"\r\n", b"\n")
                  .replace(b"__release__ = False\n", b""))
        for value in ("None", "0", "1", "'False'", "bool(0)", "False\n__release__ = True"):
            with self.subTest(value=value), self.assertRaises(InvalidPlugin):
                read_package(source + f"\n__release__ = {value}\n".encode(), "lifecycle.ovyplugin")
        for value, expected in (("True", True), ("False", False)):
            metadata, _ = read_package(source + f"\n__release__ = {value}\n".encode(), "lifecycle.ovyplugin")
            self.assertIs(metadata["release"], expected)

    def test_import_does_not_execute_plugin(self):
        manager = Manager(self.profile)
        self.addCleanup(manager.shutdown)
        source = self.root / "lifecycle.ovyplugin"
        source.write_text(EXAMPLE.read_text(encoding="utf-8") + '\nraise SystemExit("test")\n',
                          encoding="utf-8")
        plugin_id = manager.install(str(source))
        self.assertFalse(manager.snapshot()["plugins"][0]["enabled"])
        with self.assertRaisesRegex(RuntimeError, "test"):
            manager.enable(plugin_id)
        status = manager.snapshot()["plugins"][0]
        self.assertFalse(status["loaded"])
        self.assertFalse(status["enabled"])
        self.assertEqual(status["error"], "test")

    def test_restart_and_crash_recovery_keep_settings(self):
        manager = Manager(self.profile)
        try:
            plugin_id = manager.install(str(EXAMPLE))
            manager.enable(plugin_id)
            manager.save_settings(plugin_id, {"enabled_example": True, "greeting": "Сохранено", "choice": 1})
        finally:
            manager.shutdown()
        self.profile.joinpath("running").write_text(plugin_id, encoding="utf-8")
        recovery = Manager(self.profile)
        self.addCleanup(recovery.shutdown)
        self.assertTrue(recovery.snapshot()["recovery"])
        self.assertFalse(recovery.loaded)
        recovery.resume()
        recovery.resume()
        self.assertEqual(recovery.records[plugin_id]["settings"]["load_count"], 2)
        self.assertEqual(recovery.settings(plugin_id)[1]["value"], "Сохранено")
        recovery.disable(plugin_id)
        self.assertFalse(recovery.loaded)

    def test_embedded_startup_waits_for_native_client_before_registering_ui(self):
        source = self.root / "startup_ui.ovyplugin"
        source.write_text(
            'from base_plugin import BasePlugin\nfrom desktop import ui\n'
            '__id__="startup_ui"\n__name__="Startup"\n__description__="Startup UI"\n'
            '__author__="Test"\n__version__="1.0"\n__release__=False\n'
            '__platform__="desktop"\n__desktop_sdk__=2\n'
            'class StartupPlugin(BasePlugin):\n'
            '    def on_plugin_load(self):\n'
            '        ui.add_menu_item("message", "Test", lambda _: None)\n'
            '        self.set_setting("loads", self.get_setting("loads", 0) + 1)\n', encoding="utf-8")
        initial = Manager(self.profile)
        identity = initial.install(str(source))
        initial.records[identity]["enabled"] = True
        initial.save()
        initial.shutdown()
        restored = Manager(self.profile, defer_start=True)
        self.addCleanup(restored.shutdown)
        self.assertFalse(restored.loaded)
        self.assertNotIn(identity, restored.errors)
        commands = []
        restored.runtime.sender = commands.append
        restored.set_client_ready(True)
        self.assertIn(identity, restored.loaded)
        self.assertEqual(restored.records[identity]["settings"]["loads"], 1)
        self.assertTrue(any(command["method"] == "ui.menu.add" for command in commands))
        restored.set_client_ready(False)
        self.assertFalse(restored.runtime.ready.is_set())
        restored.set_client_ready(True)
        self.assertEqual(restored.records[identity]["settings"]["loads"], 1)

    def test_zip_resources_and_rollback(self):
        manager = Manager(self.profile)
        self.addCleanup(manager.shutdown)
        plugin_id = manager.install(str(EXAMPLE))
        manager.records[plugin_id]["current"].pop("share_file")
        # Simulate a pre-hash installation, whose original package wasn't kept.
        manager.records[plugin_id]["current"].pop("sha256")
        archive = self.root / "lifecycle.ovyzip"
        source = EXAMPLE.read_text(encoding="utf-8").replace("1.0.0", "2.0.0")
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr("plugin.json", '{"id": "lifecycle", "entry": "main.py"}')
            package.writestr("main.py", source + "\nfrom .resource import value\n")
            package.writestr("resource.py", "value = 5\n")
        manager.install(str(archive), plugin_id)
        manager.enable(plugin_id)
        self.assertEqual(manager.snapshot()["plugins"][0]["version"], "2.0.0")
        current = manager.records[plugin_id]["current"]
        manager.directory(current).joinpath("runtime-private.txt").write_text("local data")
        shared = Path(json.loads(manager.dispatch(json.dumps({
            "action": "share_file", "installation_id": plugin_id,
        })))["result"])
        self.assertEqual(shared.name, "lifecycle.ovyzip")
        self.assertEqual(shared.read_bytes(), archive.read_bytes())
        manager.rollback(plugin_id)
        self.assertEqual(manager.snapshot()["plugins"][0]["version"], "1.0.0")
        self.assertFalse(manager.loaded)
        legacy_file = Path(manager.share_file(plugin_id))
        self.assertEqual(legacy_file.read_bytes(), EXAMPLE.read_bytes())
        manager.remove(plugin_id)
        self.assertEqual(manager.snapshot()["plugins"], [])
        self.assertFalse(shared.exists())
        self.assertFalse(legacy_file.exists())

    def test_zip_with_windows_path_escape_is_rejected(self):
        manager = Manager(self.profile)
        self.addCleanup(manager.shutdown)
        for path in ("../escape", "c:/escape", "aux.txt", "a\\escape"):
            with self.subTest(path=path):
                archive = self.root / "lifecycle.ovyzip"
                with zipfile.ZipFile(archive, "w") as package:
                    package.writestr("plugin.json", '{"id": "lifecycle", "entry": "main.py"}')
                    package.write(EXAMPLE, "main.py")
                    package.writestr(path.replace("\\", "/"), "bad")
                if "\\" in path:
                    # ZipInfo normalizes names when writing on Windows.
                    archive.write_bytes(archive.read_bytes().replace(
                        path.replace("\\", "/").encode(), path.encode()))
                with self.assertRaises(ValueError):
                    manager.install(str(archive))
                self.assertEqual(list(manager.packages.iterdir()), [])

    @unittest.skipUnless(os.environ.get("OVYGRAM_SMOKE_EXE"), "native host not staged")
    def test_embedded_host_loads_and_restarts(self):
        executable = Path(os.environ["OVYGRAM_SMOKE_EXE"])

        def run(*requests, disabled=False):
            result = subprocess.run(
                [str(executable), str(self.profile), "disabled" if disabled else "normal"],
                input="\n".join(json.dumps(request) for request in requests) + "\n",
                text=True, encoding="utf-8", capture_output=True, timeout=30,
                cwd=self.root,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            responses = [json.loads(line) for line in result.stdout.splitlines()]
            self.assertEqual(len(responses), len(requests))
            self.assertTrue(all("result" in response for response in responses), responses)
            return [response["result"] for response in responses]

        installation_id = run({"action": "install", "path": str(EXAMPLE)})[0]
        first = run(
            {"action": "enable", "installation_id": installation_id},
            {"action": "snapshot"},
        )
        self.assertTrue(first[-1]["plugins"][0]["loaded"])
        self.assertFalse(self.profile.joinpath("running").exists())
        self.assertFalse(run({"action": "snapshot"}, disabled=True)[0]["plugins"][0]["loaded"])
        self.assertTrue(run({"action": "snapshot"})[0]["plugins"][0]["loaded"])
        state = json.loads(self.profile.joinpath("state.json").read_text(encoding="utf-8"))
        self.assertEqual(state["installations"][installation_id]["settings"]["load_count"], 2)


class InstallationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.profile = self.root / "profile"
        self.manager = Manager(self.profile)
        self.addCleanup(self.manager.shutdown)
        self.manager.runtime.sender = lambda command: None

    def copy(self, name="lifecycle.ovyplugin", version=None):
        source = self.root / "files" / name
        source.parent.mkdir(exist_ok=True)
        text = EXAMPLE.read_text(encoding="utf-8")
        source.write_text(text.replace('"1.0.0"', repr(version).replace("'", '"')) if version else text,
                          encoding="utf-8")
        return source

    def test_inspect_does_not_install(self):
        info = self.manager.inspect(str(self.copy()))
        self.assertEqual(info["metadata"]["id"], "lifecycle")
        self.assertEqual(info["package_type"], "ovyplugin")
        self.assertEqual(len(info["sha256"]), 64)
        self.assertEqual(info["installations"], [])
        self.assertEqual(self.manager.records, {})

    def test_setting_actions_keep_owner_and_reject_stale_clicks(self):
        source = self.root / "settings_actions.ovyplugin"
        source.write_text('''from base_plugin import BasePlugin
from desktop import ui
from ui.settings import Button, Header, Input, Link
__id__ = "settings_actions"
__name__ = "Settings actions"
__description__ = "Settings action test"
__author__ = "Test"
__version__ = "1.0"
__release__ = False
__platform__ = "desktop"
__desktop_sdk__ = 2
class SettingsPlugin(BasePlugin):
    def create_settings(self):
        return [
            Header("Section"),
            Button("action", "Run", self.clicked),
            Link("open_settings", "Open settings", self.open_settings, description="Also in the menu"),
            Input("name", "Name", "Default"),
        ]
    def clicked(self):
        self.set_setting("clicked", self.get_setting("clicked", 0) + 1)
    def open_settings(self):
        ui.Section("Settings content", title="Plugin settings").show()
''', encoding="utf-8")
        first = self.manager.install(str(source))
        second = self.manager.install(str(source), separate=True)
        self.manager.enable(first)
        self.manager.enable(second)
        fields = self.manager.settings(first)
        self.assertEqual([field["kind"] for field in fields], ["header", "button", "link", "input"])
        action = fields[1]["action_id"]
        link = fields[2]
        self.assertEqual(link["description"], "Also in the menu")
        with self.assertRaisesRegex(ValueError, "устарела"):
            self.manager.settings_action(second, action)
        with self.assertRaisesRegex(ValueError, "устарела"):
            self.manager.settings_action(second, link["action_id"])
        self.manager.save_settings(first, {"name": "Changed"})
        self.manager.settings_action(first, action)
        self.assertEqual(self.manager.records[first]["settings"]["clicked"], 1)
        self.assertNotIn("clicked", self.manager.records[second]["settings"])
        commands = []
        self.manager.runtime.sender = commands.append
        self.manager.runtime.ready.set()
        self.manager.settings_action(first, link["action_id"])
        self.assertEqual(len(commands), 1)
        command = commands[0]
        self.assertEqual(command["installation_id"], first)
        self.assertEqual((command["method"], command["kind"], command["title"]),
                         ("ui.show", "section", "Plugin settings"))
        self.manager.disable(first)
        self.manager.enable(first)
        for old_action in (action, link["action_id"]):
            with self.assertRaisesRegex(ValueError, "устарела"):
                self.manager.settings_action(first, old_action)

    def test_two_installations_of_one_id_are_isolated(self):
        first = self.manager.install(str(self.copy()))
        with self.assertRaisesRegex(ValueError, "отдельно"):
            self.manager.install(str(self.copy()))
        second = self.manager.install(str(self.copy()), separate=True)
        self.assertNotEqual(first, second)
        for installation_id in (first, second):
            self.manager.enable(installation_id)
        self.manager.save_settings(first, {"enabled_example": True, "greeting": "первый", "choice": 1})
        self.assertNotEqual(self.manager.settings(second)[1]["value"], "первый")
        self.manager.runtime.contexts[first].set("only_first", 1)
        self.assertNotIn("only_first", self.manager.records[second]["settings"])
        self.assertEqual({plugin["instances"] for plugin in self.manager.snapshot()["plugins"]}, {2})
        self.assertEqual(len(self.manager.inspect(str(self.copy()))["installations"]), 2)

    def test_update_keeps_the_installation(self):
        installation_id = self.manager.install(str(self.copy()))
        self.manager.enable(installation_id)
        data = self.profile / "data" / installation_id
        data.mkdir(parents=True)
        (data / "notes.txt").write_text("keep")
        updated = self.manager.install(str(self.copy(version="1.1")), installation_id)
        self.assertEqual(updated, installation_id)
        self.assertEqual(self.manager.records[installation_id]["settings"]["load_count"], 1)
        self.assertTrue((data / "notes.txt").exists())
        self.assertEqual(self.manager.describe(installation_id)["version"], "1.1")
        self.manager.remove(installation_id)
        self.assertFalse(data.exists())

    def test_replaced_files_are_not_loaded(self):
        installation_id = self.manager.install(str(self.copy()))
        version = self.manager.records[installation_id]["current"]
        entry = self.manager.directory(version) / "plugin.py"
        entry.write_text(entry.read_text(encoding="utf-8") + "\n# changed\n", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "изменён"):
            self.manager.enable(installation_id)
        self.assertNotIn(installation_id, self.manager.loaded)

    def test_legacy_state_is_migrated(self):
        self.manager.shutdown()
        legacy = self.root / "legacy"
        packages = legacy / "packages" / ("a" * 32)
        packages.mkdir(parents=True)
        source = EXAMPLE.read_text(encoding="utf-8").replace(
            '__id__ = "lifecycle"', '__id__ = "desktop.lifecycle"').replace("__release__ = False\n", "")
        (packages / "plugin.py").write_text(source, encoding="utf-8")
        (legacy / "data" / "desktop.lifecycle").mkdir(parents=True)
        (legacy / "data" / "desktop.lifecycle" / "notes.txt").write_text("keep")
        from ovygram_runtime import legacy_metadata
        metadata = legacy_metadata(source.encode(), "plugin.py")
        (legacy / "state.json").write_text(json.dumps({"desktop.lifecycle": {
            "current": {"directory": "a" * 32, "metadata": metadata, "entry": "plugin.py"},
            "previous": None, "enabled": True, "settings": {"load_count": 5},
        }}), encoding="utf-8")
        manager = Manager(legacy)
        self.addCleanup(manager.shutdown)
        manager.runtime.sender = lambda command: None
        [(installation_id, record)] = manager.records.items()
        self.assertEqual(record["plugin_id"], "desktop.lifecycle")
        self.assertIn(installation_id, manager.loaded, manager.errors)
        self.assertEqual(record["settings"]["load_count"], 6)
        self.assertTrue((legacy / "data" / installation_id / "notes.txt").exists())
        state = json.loads((legacy / "state.json").read_text(encoding="utf-8"))
        self.assertEqual(list(state["installations"]), [installation_id])

    def test_crash_marker_auto_clears_after_stable_period_and_resets_on_load(self):
        self.manager.MARKER_CLEAR_DELAY = 0.12
        first = self.manager.install(str(self.copy("first.ovyplugin")))
        second = self.manager.install(str(self.copy("second.ovyplugin")), separate=True)
        self.manager.enable(first)
        self.assertTrue(self.manager.marker.exists())
        time.sleep(0.06)
        self.manager.enable(second)
        time.sleep(0.08)
        self.assertTrue(self.manager.marker.exists())
        deadline = time.monotonic() + 1.0
        while self.manager.marker.exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertFalse(self.manager.marker.exists())
        self.manager.recovery = True
        self.manager.marker.write_text("recover", encoding="utf-8")
        self.manager._schedule_marker_clear()
        time.sleep(0.16)
        self.assertTrue(self.manager.marker.exists())

    def test_zip_case_insensitive_file_dir_collision_rejected(self):
        archive = self.root / "collision.ovyzip"
        with zipfile.ZipFile(archive, "w") as package:
            package.writestr("plugin.json", '{"id": "lifecycle", "entry": "main.py"}')
            package.write(EXAMPLE, "main.py")
            package.writestr("Sub", "file")
            package.writestr("sub/nested.py", "x = 1\n")
        with self.assertRaises(ValueError):
            self.manager.install(str(archive))

    def test_ui_image_embeds_in_plugin_dir_and_rejects_escapes_and_updates(self):
        installation_id = self.manager.install(str(self.copy()))
        self.manager.enable(installation_id)
        data_dir = self.profile / "data" / installation_id
        data_dir.mkdir(parents=True, exist_ok=True)
        icon = data_dir / "icon.png"
        raw = b"\x89PNG\r\n\x1a\npayload"
        icon.write_bytes(raw)
        outside = self.root / "secret.png"
        outside.write_bytes(b"secret")
        sent = []
        self.manager.sender = sent.append
        self.manager.host_submit({
            "id": "1", "method": "ui.show", "installation_id": installation_id,
            "tree": {"id": "n1", "type": "image", "path": str(icon)},
        })
        self.assertEqual(len(sent), 1)
        node = sent[0]["tree"]
        self.assertNotIn("path", node)
        self.assertEqual(node["data"], base64.b64encode(raw).decode("ascii"))
        for bad_path in (str(outside), str(data_dir / ".." / "secret.png"), r"\\host\share\secret.png"):
            before = len(sent)
            self.manager.host_submit({
                "id": "2", "method": "ui.show", "installation_id": installation_id,
                "tree": {"id": "n2", "type": "image", "path": bad_path},
            })
            self.assertEqual(len(sent), before, bad_path)
            self.assertIn(installation_id, self.manager.errors)
            self.manager.errors.pop(installation_id, None)
        before = len(sent)
        self.manager.host_submit({
            "id": "3", "method": "ui.update", "installation_id": installation_id,
            "props": {"path": str(icon)},
        })
        self.assertEqual(len(sent), before)
        self.assertIn(installation_id, self.manager.errors)

    def test_transient_dispatch_error_does_not_poison_plugin_error(self):
        installation_id = self.manager.install(str(self.copy()))
        self.manager.enable(installation_id)
        reply = json.loads(self.manager.dispatch(json.dumps({
            "action": "rollback", "installation_id": installation_id,
        })))
        self.assertIn("error", reply)
        self.assertEqual(self.manager.snapshot()["plugins"][0]["error"], "")

    def test_remove_succeeds_when_unload_raises(self):
        source = self.root / "bad_unload.ovyplugin"
        source.write_text('''from base_plugin import BasePlugin
__id__ = "bad_unload"
__name__ = "Bad Unload"
__description__ = "Unload failure test"
__author__ = "Test"
__version__ = "1.0"
__release__ = False
__platform__ = "desktop"
__desktop_sdk__ = 2
class BadUnloadPlugin(BasePlugin):
    def on_plugin_unload(self):
        raise RuntimeError("unload failed")
''', encoding="utf-8")
        installation_id = self.manager.install(str(source))
        self.manager.enable(installation_id)
        self.manager.remove(installation_id)
        self.assertNotIn(installation_id, self.manager.records)
        self.assertNotIn(installation_id, self.manager.loaded)
