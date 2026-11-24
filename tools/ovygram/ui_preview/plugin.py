"""Run a Desktop SDK preview with an isolated plugin host and fake client data."""

import json
import os
from pathlib import Path
import queue
import select
import shutil
import stat
import sys
from tempfile import TemporaryDirectory

SDK = Path(__file__).resolve().parents[3] / "Telegram/PluginSDK"
sys.path.insert(0, str(SDK))

from ovygram_hosts import ProcessHost
from ovygram_runtime import Manager

ACCOUNT = {"id": "1", "user_id": 1, "name": "Тестовый аккаунт", "active": True, "test_mode": True}
SAMPLE_REPLIES = {"accounts": [ACCOUNT], "current_chat": None, "compose.get_text": "",
                  "compose.get_state": None, "ui.get_widgets": []}


def send(message):
    print(json.dumps(message, ensure_ascii=False), flush=True)


def stage_dependencies(manager, requirements, folder):
    if not requirements:
        return
    if not folder or not Path(folder).is_dir():
        raise ValueError("Подготовьте библиотеки плагина и передайте каталог через --dependencies.")
    target = manager.dependencies.root / "preview"
    shutil.copytree(folder, target, symlinks=True)
    manager.dependencies.state.update(active=target.name, requirements=requirements)
    manager.dependencies.save()


def stage_images(node, roots, assets):
    if node.get("type") == "image":
        path = Path(node["path"]).resolve(strict=True)
        if not any(path.is_relative_to(root) for root in roots):
            raise ValueError("Используйте изображения из пакета или каталога данных плагина.")
        descriptor = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        with os.fdopen(descriptor, "rb") as source:
            actual = Path(os.readlink(f"/proc/self/fd/{descriptor}"))
            if not stat.S_ISREG(os.fstat(descriptor).st_mode) or not any(
                    actual.is_relative_to(root) for root in roots):
                raise ValueError("Изображение должно быть обычным файлом плагина.")
            with TemporaryDirectory(prefix="image-", dir=assets, delete=False) as folder:
                staged = Path(folder) / path.name
                with staged.open("wb") as destination:
                    shutil.copyfileobj(source, destination)
            node["path"] = str(staged)
    for child in node.get("children", ()):
        stage_images(child, roots, assets)


def run(path, selected_menu, selected_setting, dependencies):
    with TemporaryDirectory(prefix="ovygram-ui-preview-") as temporary:
        manager = Manager(Path(temporary) / "profile", defer_start=True,
                          host=lambda owner: ProcessHost(owner, allow_network=False))
        try:
            commands = queue.SimpleQueue()
            manager.sender = commands.put
            manager.set_client_ready(True)
            identity = manager.install(path)
            version = manager.records[identity]["current"]
            assets = Path(temporary) / "images"
            assets.mkdir()
            image_roots = (manager.directory(version), manager.root / "data" / identity)
            stage_dependencies(manager, version["metadata"]["requirements"], dependencies)
            manager.enable(identity)
            fields = manager.settings(identity)
            main_menu = {}

            def deliver():
                while not commands.empty():
                    command = commands.get()
                    method = command.get("method")
                    if method == "ui.menu.add" and command.get("menu") == "main":
                        main_menu[command["text"]] = command["rule_id"]
                    elif method == "ui.menu.remove":
                        for label, rule_id in list(main_menu.items()):
                            if rule_id == command["rule_id"]:
                                del main_menu[label]
                    elif method in {"ui.show", "ui.update", "ui.close"}:
                        if method == "ui.show":
                            stage_images(command["tree"], image_roots, assets)
                        send(command)
                    elif method == "python_ui":
                        manager.forward("execute_ui", None, call_id=command["call_id"])
                        manager.forward("complete", None, request={"id": command["id"], "result": None})
                    elif not method.startswith("ui.") or method.startswith("compose."):
                        reply = {"id": command["id"]}
                        if method in SAMPLE_REPLIES:
                            reply["result"] = SAMPLE_REPLIES[method]
                        else:
                            reply["error"] = {"name": "PREVIEW_OFFLINE",
                                              "description": "В предпросмотре доступны только тестовые данные."}
                        manager.forward("complete", None, request=reply)

            deliver()
            if selected_menu:
                if selected_menu not in main_menu:
                    raise ValueError(f"Пункт «{selected_menu}» не найден. Доступно: {', '.join(main_menu) or '—'}.")
                manager.forward("ui_event", None, event={
                    "type": "menu_click", "installation_id": identity, "rule_id": main_menu[selected_menu],
                    "value": {"account_id": ACCOUNT["id"]},
                })
            elif selected_setting:
                action = next((item["action_id"] for item in fields
                               if item.get("key") == selected_setting and item["kind"] in {"button", "link"}), None)
                if action is None:
                    raise ValueError(f"Кнопка или ссылка «{selected_setting}» не найдена.")
                manager.settings_action(identity, action)
            else:
                plugin = {**manager.describe(identity), "loaded": True}
                send({"method": "preview.settings", "plugin": plugin, "fields": fields})

            data = b""
            while True:
                manager.host.check()
                deliver()
                if identity in manager.errors:
                    raise RuntimeError(manager.errors[identity])
                if not select.select([sys.stdin], [], [], 0.025)[0]:
                    continue
                block = os.read(sys.stdin.fileno(), 65536)
                if not block:
                    return
                data += block
                while b"\n" in data:
                    line, data = data.split(b"\n", maxsplit=1)
                    event = json.loads(line)
                    kind = event.get("kind")
                    if kind == "setting":
                        manager.settings_action(identity, event["action_id"])
                    elif kind == "save":
                        manager.save_settings(identity, event["values"])
                    else:
                        manager.forward("ui_event", None, event={**event, "installation_id": identity})
        finally:
            manager.shutdown()


if __name__ == "__main__":
    try:
        run(*sys.argv[1:])
    except (RuntimeError, OSError, ValueError) as error:
        send({"method": "preview.error", "message": str(error)})
        sys.exit(1)
