"""The plugin host process and its OS restrictions.

The other tests run plugins inside the test process (InProcessManager).
These start the real host the client uses and check, from inside a plugin,
what it can't reach.
"""

import json
import os
from pathlib import Path
import platform
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "Telegram/PluginSDK"))
import ovygram_hosts
from ovygram_runtime import InProcessManager, Manager
from ovygram_sandbox import SYSCALLS, landlock_abi, seccomp_filter

HEADER = '''from base_plugin import BasePlugin
from hook_utils import HookResult, HookStrategy
from ui.settings import Switch

__id__ = "probe"
__name__ = "Probe"
__version__ = "1.0.0"
__author__ = "Tests"
__description__ = "Test plugin."
__platform__ = "desktop"
__desktop_sdk__ = 1
__requirements__ = []

'''

PROBE = HEADER + '''
import ctypes
import os
import socket
from file_utils import get_plugin_data_dir, get_plugin_dir

CLIENT = {client}
SECRET = {secret!r}
ABSTRACT = {abstract!r}


def attempt(action):
    try:
        action()
        return "ok"
    except BaseException as error:
        return type(error).__name__


def connect():
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.connect("\\0" + ABSTRACT)


def read_memory():
    libc = ctypes.CDLL(None, use_errno=True)
    buffer = ctypes.create_string_buffer(8)
    local = (ctypes.c_void_p * 2)(ctypes.addressof(buffer), 8)
    remote = (ctypes.c_void_p * 2)(ctypes.addressof(buffer), 8)
    if libc.process_vm_readv(CLIENT, local, 1, remote, 1, 0) < 0:
        raise OSError(ctypes.get_errno())


def trace():
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.ptrace(16, CLIENT, 0, 0) < 0:
        raise OSError(ctypes.get_errno())


class Probe(BasePlugin):
    def on_plugin_load(self):
        print("printed-by-plugin")
        self.set_setting("probe", {{
            "secret": attempt(lambda: open(SECRET).read()),
            "package": attempt(lambda: (get_plugin_dir() / "written").write_text("x")),
            "data": attempt(lambda: (get_plugin_data_dir() / "written").write_text("x")),
            "client_proc": os.path.exists(f"/proc/{{CLIENT}}"),
            "memory": attempt(read_memory),
            "trace": attempt(trace),
            "abstract": attempt(connect),
            "namespace": attempt(lambda: os.unshare(os.CLONE_NEWUSER)),
            "display": sorted(key for key in os.environ
                              if key in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS",
                                         "XAUTHORITY", "XDG_RUNTIME_DIR")),
            "session": os.path.exists("/run/user"),
            "bridge": attempt(lambda: __import__("_ovygram_native")),
        }})
'''

BEHAVIOUR = HEADER + '''
import os
import time


class Behaviour(BasePlugin):
    def on_plugin_load(self):
        self.add_hook("before_updates", self.updates)

    def updates(self, value):
        return HookResult(HookStrategy.CANCEL, reason="stopped by plugin")

    def create_settings(self):
        mode = self.get_setting("mode", "")
        if mode == "exit":
            os._exit(3)
        if mode == "sleep":
            time.sleep(60)
        if mode == "garbage":
            for descriptor in range(3, 16):
                try:
                    os.write(descriptor, b"\\xff\\xff\\xff\\xff")
                except OSError:
                    pass
            time.sleep(60)
        return [Switch("flag", "Flag")]
'''


WINDOWS_PROBE = HEADER + '''
import ctypes
from file_utils import get_plugin_data_dir

CLIENT = {client}
OUTSIDE = {outside!r}


def attempt(action):
    try:
        action()
        return "ok"
    except BaseException as error:
        return type(error).__name__


def open_client():
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = ctypes.c_void_p
    # PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE
    if not kernel32.OpenProcess(0x0038, False, CLIENT):
        raise OSError(ctypes.get_last_error())


class Probe(BasePlugin):
    def on_plugin_load(self):
        self.set_setting("probe", {{
            "outside": attempt(lambda: open(OUTSIDE, "w").close()),
            "data": attempt(lambda: (get_plugin_data_dir() / "written").write_text("x")),
            "client": attempt(open_client),
        }})
'''


SPOOFER = HEADER + '''
OTHER = {other!r}


class Spoofer(BasePlugin):
    def on_plugin_load(self):
        client = self._desktop_context.manager.client
        own = self._desktop_context.installation_id
        client.report_error(OTHER, "Плагин повреждён. Установите исправление: https://example.org")
        client.submit({{"id": "1", "method": "ui.palette", "installation_id": OTHER, "reset": True}})
        client.submit({{"id": "2", "method": "ui.palette", "installation_id": own, "reset": True}})

    def create_settings(self):
        self._desktop_context.manager.client.report_error(
            self._desktop_context.installation_id, "first line\\n\\nsecond   line " + "x" * 2000)
        return []
'''


def sandbox_works():
    bwrap = shutil.which("bwrap")
    if not sys.platform.startswith("linux") or bwrap is None or platform.machine() not in SYSCALLS:
        return False
    return subprocess.run([bwrap, "--unshare-user", "--unshare-pid", "--ro-bind", "/", "/", "true"],
                          capture_output=True).returncode == 0


class SeccompTests(unittest.TestCase):
    def test_filter_is_whole_instructions_for_each_architecture(self):
        for machine in SYSCALLS:
            program = seccomp_filter(machine)
            self.assertEqual(len(program) % 8, 0)
            self.assertLess(len(program) // 8, 256)

    @unittest.skipUnless(platform.machine() == "x86_64"
                         and Path("/usr/include/asm/unistd_64.h").is_file(), "needs kernel headers")
    def test_numbers_match_the_kernel_headers(self):
        text = Path("/usr/include/asm/unistd_64.h").read_text()
        header = {name: int(number) for name, number in re.findall(r"#define __NR_(\w+) (\d+)", text)}
        for name, number in SYSCALLS["x86_64"][1].items():
            self.assertEqual(header[name], number, name)


@unittest.skipUnless(sandbox_works(), "needs Linux with bubblewrap and user namespaces")
class PluginHostTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.commands = []

    def manager(self, kind=Manager):
        manager = kind(self.root / "profile")
        self.addCleanup(manager.shutdown)
        if kind is Manager:
            manager.sender = self.commands.append
        return manager

    def install(self, manager, source):
        path = self.root / f"{uuid.uuid4().hex}.ovyplugin"
        path.write_text(source, encoding="utf-8")
        return manager.install(str(path))

    def probe(self, kind):
        secret = self.root / "secret.txt"
        secret.write_text("secret")
        name = "ovygram-test-" + uuid.uuid4().hex
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(listener.close)
        listener.bind("\0" + name)
        listener.listen(1)
        manager = self.manager(kind)
        identity = self.install(manager, PROBE.format(
            client=os.getpid(), secret=str(secret), abstract=name))
        manager.enable(identity)
        return manager, manager.records[identity]["settings"]["probe"]

    def test_plugin_cannot_reach_the_client_or_user_files(self):
        manager, probe = self.probe(Manager)
        report = manager.snapshot()["isolation"]
        self.assertEqual((report["mode"], report["active"]), ("process", True))
        self.assertEqual(report["network"], landlock_abi() >= 6)
        self.assertEqual(probe, {
            "secret": "FileNotFoundError", "package": "OSError", "data": "ok",
            "client_proc": False, "memory": "OSError", "trace": "OSError",
            "abstract": "PermissionError" if report["network"] else "ConnectionRefusedError",
            "namespace": "PermissionError", "display": [], "session": False,
            "bridge": "ModuleNotFoundError",
        })
        manager.shutdown()
        self.assertIn("host: printed-by-plugin", (self.root / "profile/plugins.log").read_text("utf-8"))

    def test_the_same_probe_reaches_everything_without_the_host(self):
        # Shows what the probe detects: in-process plugins are not confined.
        _, probe = self.probe(InProcessManager)
        self.assertEqual((probe["secret"], probe["package"], probe["client_proc"], probe["abstract"]),
                         ("ok", "ok", True, "ok"))

    def test_hooks_settings_and_commands_cross_the_process_boundary(self):
        manager = self.manager()
        identity = self.install(manager, BEHAVIOUR)
        self.assertEqual(manager.hook_mask, 0)
        manager.enable(identity)
        self.assertEqual(manager.hook_mask, 1 << 3)
        self.assertEqual(manager.settings(identity), [
            {"key": "flag", "text": "Flag", "kind": "switch", "value": False}])
        manager.save_settings(identity, {"flag": True})
        self.assertIs(manager.settings(identity)[0]["value"], True)
        reply = json.loads(manager.dispatch(json.dumps({"action": "native_hook", "request": {
            "point": "before_updates", "account_id": "1",
            # updatesTooLong#e317af7e
            "data": "fq8X4w=="}})))
        self.assertEqual(reply, {"error": "stopped by plugin", "cancelled": True})
        manager.disable(identity)
        self.assertEqual(manager.hook_mask, 0)
        self.assertEqual([command["method"] for command in self.commands], ["ui.close_plugin"])
        self.assertEqual(self.commands[0]["installation_id"], identity)

    def test_host_cannot_speak_for_a_plugin_that_does_not_run_in_it(self):
        manager = self.manager()
        other = self.install(manager, BEHAVIOUR)
        spoofer = self.install(manager, SPOOFER.replace('__id__ = "probe"', '__id__ = "spoofer"')
                               .format(other=other))
        manager.enable(spoofer)
        self.assertNotIn(other, manager.errors)
        self.assertEqual([(command["id"], command["installation_id"]) for command in self.commands],
                         [("2", spoofer)])
        self.assertEqual(manager.settings(spoofer), [])
        plugins = {plugin["installation_id"]: plugin for plugin in manager.snapshot()["plugins"]}
        self.assertEqual((plugins[other]["error"], plugins[other]["error_from_plugin"]), ("", False))
        error = plugins[spoofer]["error"]
        self.assertTrue(error.startswith("first line second line xxx"), error)
        self.assertEqual((len(error), plugins[spoofer]["error_from_plugin"]), (500, True))

    def stopped(self, mode, pattern):
        manager = self.manager()
        identity = self.install(manager, BEHAVIOUR)
        manager.enable(identity)
        manager.set_setting(identity, "mode", mode)
        with self.assertRaisesRegex(ovygram_hosts.HostError, pattern):
            manager.settings(identity)
        self.assertEqual(manager.loaded, {})
        self.assertEqual(manager.hook_mask, 0)
        self.assertTrue(manager.snapshot()["recovery"])
        self.assertRegex(manager.errors[identity], pattern)
        # Requests and updates go on unchanged while no plugin runs.
        request = {"point": "before_updates", "account_id": "1", "data": "fq8X4w=="}
        self.assertEqual(json.loads(manager.dispatch(json.dumps({
            "action": "native_hook", "request": request})))["result"], {"data": "fq8X4w==", "error": None})
        manager.set_setting(identity, "mode", "")
        manager.resume()
        self.assertIn(identity, manager.loaded)
        self.assertEqual(len(manager.settings(identity)), 1)

    def test_host_crash_stops_plugins_until_resumed(self):
        self.stopped("exit", "завершился")

    def test_corrupted_channel_stops_only_the_host(self):
        self.stopped("garbage", "завершился")

    def test_silent_plugin_is_stopped_after_the_timeout(self):
        previous = ovygram_hosts.TIMEOUTS["settings"]
        ovygram_hosts.TIMEOUTS["settings"] = 1
        self.addCleanup(ovygram_hosts.TIMEOUTS.__setitem__, "settings", previous)
        self.stopped("sleep", "не ответили")


@unittest.skipUnless(sys.platform == "win32", "needs Windows")
class WindowsHostTests(unittest.TestCase):
    def test_plugin_cannot_open_the_client_or_write_user_files(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        manager = Manager(root / "profile")
        self.addCleanup(manager.shutdown)
        manager.sender = lambda command: None
        path = root / "probe.ovyplugin"
        path.write_text(WINDOWS_PROBE.format(client=os.getpid(), outside=str(root / "outside.txt")),
                        encoding="utf-8")
        identity = manager.install(str(path))
        manager.enable(identity)
        report = manager.snapshot()["isolation"]
        self.assertEqual((report["mode"], report["active"], report["reason"]), ("process", True, ""))
        self.assertEqual(manager.records[identity]["settings"]["probe"],
                         {"outside": "PermissionError", "data": "ok", "client": "OSError"})
        self.assertFalse((root / "outside.txt").exists())


if __name__ == "__main__":
    unittest.main()
