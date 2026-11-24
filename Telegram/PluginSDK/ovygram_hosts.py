"""How the trusted manager reaches the code that runs plugins.

ProcessHost starts ovygram_host.py as a restricted child process and is what
the client uses. Plugin code never runs in the process of the client, so it
can't reach the manager, its windows or the memory of the application.

LocalHost runs the same executor inside the calling process. It gives no
protection and exists for the SDK tests and tools.
"""

import itertools
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time

from hook_utils import HookCancelled
from ovygram_channel import Channel, ChannelClosed
from ovygram_deps import python_command
from ovygram_sandbox import SandboxError, confine, release, wrap

START_TIMEOUT = 30
# Seconds the client waits for plugin code before it stops the host.
TIMEOUTS = {"load": 120, "unload": 30, "settings": 30, "settings_action": 30, "shutdown": 10}
HOOK_TIMEOUT = 10
MAX_ERROR = 4096
MAX_LOG = 16384
MASK_BITS = (1 << 17) - 1


class HostError(RuntimeError):
    pass


class PluginError(HostError):
    """An exception of plugin code; its text is written by the plugin."""


def _text(value, limit):
    return value[:limit] if isinstance(value, str) else ""


class Session:
    def __init__(self, process):
        self.process = process
        self.channel = Channel(process.stdout, process.stdin)
        self.inbox = queue.Queue()
        self.threads = []
        self.output = []  # The last lines of the error stream.
        self.dead = False
        self.guard = None  # Of the sandbox, released after the process.


class ProcessHost:
    def __init__(self, manager, *, allow_network=True):
        self.manager = manager
        self.allow_network = allow_network
        self.session = None
        self.calls = itertools.count(1)
        self.report = {"active": False, "network": True, "reason": ""}

    @property
    def running(self):
        return self.session is not None

    def status(self):
        return {"mode": "process", "running": self.running, **self.report}

    # Start and stop.

    def start(self):
        manager = self.manager
        python, python_environment, bundled = python_command(manager.python_dir)
        if sys.platform.startswith("linux") and not bundled:
            python = str(Path(python).resolve())
        data = manager.root / "data"
        data.mkdir(exist_ok=True)
        environment = {**python_environment, "PYTHONUTF8": "1", "PYTHONNOUSERSITE": "1",
                       "PYTHONDONTWRITEBYTECODE": "1"}
        options = {"bridge": manager.bridge, "dependencies": manager.dependencies.active()}
        read_only = [manager.python_dir, manager.packages, manager.dependencies.root]
        try:
            sandboxed = {**environment, "HOME": str(data), "TMPDIR": "/tmp", "LANG": "C.UTF-8",
                         "PATH": "/usr/bin:/bin"}
            flags = ["-s", "-S"] if bundled else ["-I", "-S"]
            command, descriptors, report = wrap(
                # Resolved like python_dir: the sandbox binds real paths.
                [python, *flags, str(Path(__file__).resolve().with_name("ovygram_host.py"))],
                sandboxed, read_only=read_only, writable=[data], directory=data,
                allow_network=self.allow_network)
        except SandboxError as error:
            self.report = {"active": False, "network": False, "reason": str(error)}
            raise
        options["scope"] = bool(report.get("scoped"))
        command.append(json.dumps(options))
        if descriptors:
            environment = {}  # bubblewrap sets the environment of the host itself.
        else:
            environment = {**{key: value for key, value in os.environ.items()
                              if not key.startswith("PYTHON")}, **environment}
            if report["active"]:
                # The profile of the user isn't writable for the host.
                temporary = data / "tmp"
                temporary.mkdir(exist_ok=True)
                environment.update(TEMP=str(temporary), TMP=str(temporary))
        # Where the restrictions are applied to the created process, it
        # stays suspended until confine() has applied them.
        suspended = report.pop("creation_flags", 0)
        try:
            process = subprocess.Popen(
                command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                env=environment, cwd=str(data), pass_fds=descriptors,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0) | suspended)
        finally:
            for descriptor in descriptors:
                os.close(descriptor)
        session = Session(process)
        try:
            if suspended:
                session.guard = confine(process)
        except Exception as error:
            # Also an error of the restricting code itself: the process is
            # still suspended and must not be left behind or resumed.
            process.kill()
            self.stop(session)
            self.report = {"active": False, "network": False, "reason": str(error)}
            if isinstance(error, SandboxError):
                raise
            raise SandboxError(f"Изоляция плагинов: {error}") from error
        for target in (self.read, self.read_errors):
            thread = threading.Thread(target=target, args=(session,), name="Ovygram plugin host",
                                      daemon=True)
            session.threads.append(thread)
            thread.start()
        try:
            hello = session.inbox.get(timeout=START_TIMEOUT)
        except queue.Empty:
            hello = None
        if hello is None or hello.get("event") != "hello" or bool(hello.get("scoped")) != options["scope"]:
            self.stop(session)
            detail = " ".join(session.output[-4:])
            reason = "Процесс плагинов не запустился." + (" " + detail if detail else "")
            self.report = {"active": False, "network": False, "reason": reason}
            raise HostError(reason)
        self.report = report
        self.session = session

    def stop(self, session):
        process = session.process
        try:
            process.stdin.close()
        except OSError:
            pass
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        for thread in session.threads:
            thread.join(timeout=2)
        for stream in (process.stdout, process.stderr):
            try:
                stream.close()
            except OSError:
                pass
        release(session.guard)
        session.guard = None

    def close(self):
        session = self.session
        if session is None:
            return
        try:
            self.call("shutdown")
        except (HostError, HookCancelled):
            pass
        if self.session is session:
            self.session = None
            self.stop(session)

    def fail(self, reason):
        """The host is gone or must go; on the main thread."""
        session, self.session = self.session, None
        if session is not None:
            session.process.kill()
            self.stop(session)
        self.manager.host_exited(reason)

    def check(self):
        session = self.session
        if session is not None and session.dead:
            self.fail(self.exit_reason(session))

    @staticmethod
    def exit_reason(session):
        detail = " ".join(session.output[-4:])
        return "Процесс плагинов завершился." + (" " + detail if detail else "")

    # Threads of a session.

    def read(self, session):
        try:
            message = session.channel.receive()
            if message.get("event") != "hello":
                raise ChannelClosed("Нет приветствия.")
            session.inbox.put(message)
            while True:
                message = session.channel.receive()
                if "event" in message:
                    self.event(message)
                else:
                    session.inbox.put(message)
        except Exception:
            # Also a malformed event: the host is no longer the SDK's code.
            pass
        session.dead = True
        session.process.kill()
        session.inbox.put(None)

    def read_errors(self, session):
        manager = self.manager
        try:
            for line in session.process.stderr:
                text = line.decode("utf-8", "replace").rstrip()[:MAX_LOG]
                session.output = [*session.output[-7:], text]
                manager.logger.info("host: %s", text)
        except (OSError, ValueError):
            pass

    def event(self, message):
        manager = self.manager
        kind = message["event"]
        if kind == "submit":
            manager.host_submit(message["command"])
        elif kind == "log":
            level = message["level"]
            if type(level) is not int:
                raise ValueError(kind)
            manager.logger.log(min(max(level, 10), 50), "%s", _text(message["text"], MAX_LOG))
        elif kind == "error":
            manager.host_error(message["installation_id"], _text(message["message"], MAX_ERROR))
        elif kind == "mask":
            if type(message["value"]) is not int:
                raise ValueError(kind)
            manager.hook_mask = message["value"] & MASK_BITS
        elif kind == "restart_required":
            manager.restart_required = True
        elif kind == "commands":
            manager.host_commands(message["installation_id"], message["commands"])
        elif kind == "command_suggestions":
            manager.host_suggestions(message["token"], message["items"])
        else:
            raise ValueError(kind)

    # Calls.

    def call(self, action, **params):
        """Runs an action of the executor and returns its result. Raises
        HostError with the message of the plugin's exception, HookCancelled
        for a cancelling hook."""
        self.manager.check_thread()
        self.check()
        if self.session is None:
            self.start()
        session = self.session
        identity = next(self.calls)
        timeout = TIMEOUTS.get(action, HOOK_TIMEOUT)
        deadline = time.monotonic() + timeout
        try:
            session.channel.send({"call": identity, "action": action, "params": params})
        except ChannelClosed:
            reason = self.exit_reason(session)
            self.fail(reason)
            raise HostError(reason) from None
        while True:
            try:
                message = session.inbox.get(timeout=max(deadline - time.monotonic(), 0))
            except queue.Empty:
                reason = f"Плагины не ответили за {timeout} с ({action}). Процесс плагинов остановлен."
                self.fail(reason)
                raise HostError(reason) from None
            if message is None:
                reason = self.exit_reason(session)
                self.fail(reason)
                raise HostError(reason)
            if "call" in message:
                self.answer(session, message)
            elif message.get("reply") == identity:
                if "error" in message:
                    text = _text(message["error"], MAX_ERROR) or "Ошибка плагина."
                    if message.get("cancelled"):
                        raise HookCancelled(text)
                    raise PluginError(text)
                return message.get("result")

    def answer(self, session, message):
        """A request of plugin code that needs the main thread of the client."""
        reply = {"reply": message["call"]}
        try:
            if message.get("action") != "set_setting":
                raise ValueError("Неизвестный запрос процесса плагинов.")
            self.manager.host_set_setting(**message["params"])
        except Exception as error:
            reply["error"] = str(error) or type(error).__name__
        session.channel.send(reply)


class LocalClient:
    submit = None  # The executor then uses the in-process bridge.

    def __init__(self, manager):
        self.manager = manager
        self.logger = manager.logger

    def report_error(self, installation_id, message):
        self.manager.host_error(installation_id, message)

    def set_setting(self, installation_id, key, value, account_id):
        self.manager.host_set_setting(installation_id, key, value, account_id)

    def mask_changed(self, mask):
        self.manager.hook_mask = mask

    def restart_required(self):
        self.manager.restart_required = True

    def commands_changed(self, installation_id, commands):
        self.manager.host_commands(installation_id, commands)

    def command_suggestions(self, token, items):
        self.manager.host_suggestions(token, items)


class LocalHost:
    running = True

    def __init__(self, manager):
        from ovygram_executor import Executor, LoadError
        self.manager = manager
        self.load_error = LoadError
        self.executor = Executor(LocalClient(manager))

    def status(self):
        return {"mode": "in_process", "running": True, "active": False, "network": True,
                "reason": "Плагины исполняются в процессе клиента."}

    def check(self):
        pass

    def call(self, action, **params):
        # Through JSON as between processes: neither side keeps a reference
        # to an object of the other.
        try:
            result = self.executor.dispatch(action, json.loads(json.dumps(params)))
        except self.load_error as error:
            raise PluginError(str(error)) from error
        return json.loads(json.dumps(result))

    def close(self):
        self.executor.dispatch("shutdown", {})
