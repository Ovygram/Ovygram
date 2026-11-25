"""Native components supplied by an installation. Only the trusted manager uses this module."""
import hashlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import uuid
import zipfile

from ovygram_deps import python_command
from ovygram_package import MAX_PACKAGE, archive_path
from ovygram_sandbox import SandboxError, confine, python_read_only_mounts, release, wrap

PROXY_ROUTES = ("default", "api", "files", "pages", "mini_apps")


def _send_config(pipe, data):
    try:
        with pipe:
            pipe.write(data)
    except (BrokenPipeError, OSError):
        # A child can exit or be stopped without reading its configuration.
        pass


def _log_stderr(pipe, logger, owner):
    try:
        with pipe:
            for _ in range(32):
                line = pipe.readline(4096)
                if not line:
                    break
                text = line.decode("utf-8", "replace").rstrip()
                if text:
                    logger.info("helper %s: %s", owner[:8], text)
    except (OSError, ValueError):
        pass


class Helpers:
    def __init__(self, manager):
        self.manager = manager
        self.children = {}
        self.offers = {}
        self.pending_cleanup = []
        root = getattr(manager, "root", None)
        if isinstance(root, Path) and root.is_dir():
            for entry in root.glob("helper-*"):
                if entry.is_dir() and not entry.is_symlink():
                    try:
                        shutil.rmtree(entry)
                    except OSError:
                        pass

    @staticmethod
    def _terminate(process, guard):
        if guard is not None:
            from ovygram_sandbox_windows import terminate_helper_job
            try:
                terminate_helper_job(guard)
            finally:
                release(guard)  # Also kills descendants if the wait timed out.
        if process.poll() is None:
            process.kill()
        process.wait()

    def _cleanup(self, directory, *, remaining=30):
        try:
            directory.cleanup()
        except OSError as error:
            # Windows can retain an executable mapping briefly after its job
            # exits. Access denied and sharing violations do not mean the
            # component is still running or can still be selected.
            if remaining and getattr(error, "winerror", None) in (5, 32, 33):
                self.pending_cleanup.append((time.monotonic() + 1, directory, remaining - 1))
            else:
                self.manager.logger.warning("Не удалось удалить временный каталог компонента: %s", error)

    def stop(self, owner):
        removed = self.offers.pop(owner, None)
        browser = self.offers.pop(owner + ":http", None)
        child = self.children.pop(owner, None)
        try:
            if child:
                process, guard, directory = child
                try:
                    self._terminate(process, guard)
                finally:
                    self._cleanup(directory)
        finally:
            if removed or browser:
                self.manager.notify({"id": uuid.uuid4().hex, "method": "proxy.refresh"})

    def poll(self):
        pending, self.pending_cleanup = self.pending_cleanup, []
        for retry_at, directory, remaining in pending:
            if retry_at <= time.monotonic():
                self._cleanup(directory, remaining=remaining)
            else:
                self.pending_cleanup.append((retry_at, directory, remaining))
        for owner, (process, _, _) in list(self.children.items()):
            if process.poll() is not None:
                self.stop(owner)

    def start(self, owner, resource, arguments, config):
        if owner in self.children:
            raise ValueError("Сначала остановите работающий компонент.")
        if not isinstance(resource, str) or len(resource) > 240:
            raise ValueError("Недопустимое имя компонента.")
        resource = archive_path(resource).as_posix()
        if not isinstance(arguments, list) or len(arguments) > 32 or not all(
                isinstance(arg, str) and len(arg) <= 1024 and "\x00" not in arg for arg in arguments):
            raise ValueError("Недопустимые аргументы компонента.")
        if not isinstance(config, str) or len(config.encode()) > 256 * 1024:
            raise ValueError("Конфигурация превышает 256 КиБ.")
        version = self.manager.records[owner]["current"]
        if version["package_type"] != "ovyzip" or not version.get("sha256"):
            raise ValueError("Компонент должен входить в установленный .ovyzip.")
        with self.manager.original(version).open("rb") as stream:
            package = stream.read(MAX_PACKAGE + 1)
        if len(package) > MAX_PACKAGE or hashlib.sha256(package).hexdigest() != version["sha256"]:
            raise ValueError("Контрольная сумма установленного архива изменилась.")
        directory = tempfile.TemporaryDirectory(dir=self.manager.root, prefix="helper-")
        process = guard = None
        try:
            folder = Path(directory.name)
            if os.name == "nt":
                from ovygram_sandbox_windows import grant_helper_read
                grant_helper_read(folder)
            binary = folder / ("component.exe" if os.name == "nt" else "component")
            with zipfile.ZipFile(io.BytesIO(package)) as archive, archive.open(resource) as source:
                with binary.open("xb") as target:
                    shutil.copyfileobj(source, target)
            with binary.open("rb") as stream:
                magic = stream.read(4)
            if not (magic.startswith(b"MZ") if os.name == "nt" else magic == b"\x7fELF"):
                raise ValueError("Формат компонента не подходит для этой ОС.")
            if os.name != "nt":
                binary.chmod(0o500)
            python, environment, bundled = python_command(self.manager.python_dir)
            if sys.platform.startswith("linux") and not bundled:
                python = str(Path(python).resolve())
            launcher = Path(__file__).resolve().with_name("ovygram_helper_launcher.py")
            # The trusted launcher applies the final restrictions before executing any package bytes.
            args = [python, *(["-s", "-S"] if bundled else ["-I", "-S"]), str(launcher)]
            read_only = [*python_read_only_mounts(python, bundled, self.manager.python_dir), folder]
            allow_network = getattr(self.manager.host, "allow_network", True)
            argv, descriptors, report = wrap(
                args, environment, read_only=read_only, directory=folder, allow_network=allow_network)
            try:
                if not report["active"]:
                    raise SandboxError("Для запуска компонента нужна системная песочница.")
                if os.name == "nt":
                    environment["SystemRoot"] = os.environ.get("SystemRoot", r"C:\Windows")
                argv.extend([str(int(bool(report.get("scoped")))), str(binary), *arguments])
                process = subprocess.Popen(
                    argv, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                    close_fds=True, pass_fds=descriptors, env={} if descriptors else environment,
                    creationflags=report.get("creation_flags", 0) | getattr(subprocess, "CREATE_NO_WINDOW", 0))
                if report.get("creation_flags"):
                    guard = confine(process)
                # A component that does not read stdin must not block the manager.
                threading.Thread(target=_send_config, args=(process.stdin, config.encode()),
                                 name="Ovygram component input", daemon=True).start()
                threading.Thread(target=_log_stderr, args=(process.stderr, self.manager.logger, owner),
                                 name="Ovygram component log", daemon=True).start()
            finally:
                for descriptor in descriptors:
                    os.close(descriptor)
            self.children[owner] = process, guard, directory
        except BaseException:
            try:
                if process is not None:
                    self._terminate(process, guard)
                    process.stdin.close()
                    if process.stderr is not None:
                        process.stderr.close()
            finally:
                self._cleanup(directory)
            raise

    def command(self, installation_id, operation, params):
        owner = installation_id
        self.poll()
        if owner not in self.manager._loaded or not self.manager.host.status()["active"]:
            raise ValueError("Нужен активный плагин в системной песочнице.")
        if operation == "start":
            self.start(owner, params.get("resource"), params.get("arguments"), params.get("config"))
        elif operation == "stop":
            self.stop(owner)
        elif operation == "status":
            return owner in self.children
        elif operation == "network_available":
            return self.manager.host.status()["network"]
        elif operation in ("offer", "offer_http"):
            if owner not in self.children or not self.manager.host.status()["network"]:
                raise ValueError("Сетевой компонент недоступен.")
            port, user, password = (params.get(key) for key in ("port", "user", "password"))
            if type(port) is not int or not 1024 <= port <= 65535 or not all(
                    isinstance(v, str) and v.isascii() and v.isprintable() and 1 <= len(v) <= 255
                    for v in (user, password)):
                raise ValueError("Укажите локальный порт и учётные данные прокси.")
            protocol = "http" if operation == "offer_http" else "socks5"
            supported = ("pages", "mini_apps") if protocol == "http" else ("default", "api", "files")
            scopes = params.get("routes", list(supported))
            label = params.get("label", "")
            if not isinstance(label, str) or len(label) > 24 or any(not (c.isalnum() or c in " -_") for c in label):
                raise ValueError("Недопустимое название для индикатора прокси.")
            if (not isinstance(scopes, list) or not 0 < len(scopes) <= len(supported)
                    or any(scope not in supported for scope in scopes) or len(set(scopes)) != len(scopes)):
                raise ValueError("Недопустимые области маршрутизации для выбранного протокола.")
            key = owner + ":http" if protocol == "http" else owner
            self.offers[key] = dict(token=uuid.uuid4().hex, port=port, user=user, password=password,
                                    protocol=protocol, routes=scopes, label=label.strip(),
                                    restore=params.get("restore") is True,
                                    plugin_id=self.manager.records[owner]["plugin_id"], owner=owner)
            self.manager.notify({"id": uuid.uuid4().hex, "method": "proxy.refresh"})
        else:
            raise ValueError("Неизвестная операция компонента.")
        return True
