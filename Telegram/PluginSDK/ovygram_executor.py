"""The side of the plugin system that runs plugin code.

It lives in the plugin host process (ovygram_host.py). The trusted manager
of the client (ovygram_runtime.py) checks a package and sends its source
here; nothing in this module decides what may be installed or started.

The client is reached through an object with these members:
    submit(command)         a command of the Desktop SDK, None to use the
                            in-process bridge;
    logger                  a logging.Logger;
    report_error(installation_id, message)
    set_setting(installation_id, key, value, account_id)
    mask_changed(mask)
    commands_changed(installation_id, commands)
    command_suggestions(token, items)
    restart_required()
"""

import base64
import copy
import importlib.util
import inspect
from pathlib import Path
import sys
import threading
import uuid

from base_plugin import BasePlugin
from desktop._runtime import Runtime
from ovygram_deps import Dependencies
from ui.settings import Button, Header, Input, Link, Selector, Switch


class LoadError(RuntimeError):
    pass


def account_key(account_id):
    result = str(account_id)
    if not result.isdecimal():
        raise ValueError("Некорректный ID аккаунта.")
    return result


class PluginContext:
    def __init__(self, manager, installation_id, plugin_id, directory, data_directory):
        self.manager = manager
        self.installation_id = installation_id
        self.plugin_id = plugin_id
        self.directory = directory
        self.data_directory = data_directory
        self.active = True

    def get(self, key, default, account_id=None):
        self.check_active()
        stored = self.manager.settings_of[self.installation_id]
        settings = stored["settings"] if account_id is None else stored["account_settings"].get(
            account_key(account_id), {})
        return copy.deepcopy(settings.get(key, default))

    def set(self, key, value, account_id=None):
        self.check_active()
        self.manager.check_thread()
        self.manager.set_setting(self.installation_id, key, value, account_id)

    def log(self, message):
        self.check_active()
        self.manager.logger.info("%s: %s", self.manager.label(self.installation_id), message)

    def check_active(self):
        if not self.active:
            raise RuntimeError("Плагин уже выгружен.")


class Executor:
    def __init__(self, client):
        self.client = client
        self.owner = threading.get_ident()
        self.logger = client.logger
        self.loaded = {}
        self.settings_of = {}
        self.setting_actions = {}
        self._restart_required = False
        self.runtime = Runtime(self)
        self.runtime.sender = client.submit
        self.actions = {
            "load": self.load, "unload": self.unload, "settings": self.settings,
            "settings_action": self.settings_action, "update_settings": self.update_settings,
            "dependencies_path": self.dependencies_path,
            "complete": self.runtime.complete, "is_pending": self.runtime.is_pending,
            "bridge_ready": lambda: self.runtime.ready.set(),
            "bridge_lost": lambda: self.runtime.ready.clear(),
            "native_hook": self.runtime.native_hook, "native_event": self.runtime.native_event,
            "execute_ui": self.runtime.execute_ui, "ui_event": self.runtime.ui_event,
            "command": self.runtime.run_command, "command_suggest": self.runtime.suggest_command,
            "shutdown": self.shutdown,
        }

    def dispatch(self, action, params):
        self.check_thread()
        return self.actions[action](**params)

    def check_thread(self):
        if threading.get_ident() != self.owner:
            raise RuntimeError("Настройки плагина доступны только в главном потоке.")

    @property
    def restart_required(self):
        return self._restart_required

    @restart_required.setter
    def restart_required(self, value):
        self._restart_required = value
        if value:
            self.client.restart_required()

    def hook_mask_changed(self, mask):
        self.client.mask_changed(mask)

    def commands_changed(self, installation_id, commands):
        self.client.commands_changed(installation_id, commands)

    def command_suggestions(self, token, items):
        self.client.command_suggestions(token, items)

    def label(self, installation_id):
        context = self.runtime.contexts.get(installation_id)
        return f"{context.plugin_id}[{installation_id[:8]}]" if context else installation_id

    def report_error(self, installation_id, message):
        self.logger.exception("%s: %s", self.label(installation_id), message)
        self.client.report_error(installation_id, message)

    def call(self, installation_id, method, *args):
        return self.runtime.call(self.runtime.contexts[installation_id], method, *args)

    def set_setting(self, installation_id, key, value, account_id):
        self.client.set_setting(installation_id, key, value, account_id)
        stored = self.settings_of[installation_id]
        if account_id is None:
            stored["settings"][key] = copy.deepcopy(value)
        else:
            stored["account_settings"].setdefault(account_key(account_id), {})[key] = copy.deepcopy(value)

    def update_settings(self, installation_id, settings, account_settings):
        if installation_id in self.settings_of:
            self.settings_of[installation_id] = {"settings": settings, "account_settings": account_settings}

    @staticmethod
    def dependencies_path(directory, replace=None):
        Dependencies.add_to_path(directory, replace)

    def load(self, installation_id, plugin_id, namespace, directory, entry, source, name,
             data_directory, settings, account_settings):
        """Runs the source the client has checked; raises LoadError."""
        if installation_id in self.loaded:
            raise LoadError("Плагин уже загружен.")
        directory = Path(directory)
        context = PluginContext(self, installation_id, plugin_id, directory, Path(data_directory))
        self.runtime.contexts[installation_id] = context
        self.settings_of[installation_id] = {"settings": settings, "account_settings": account_settings}
        try:
            path = directory / entry
            spec = importlib.util.spec_from_loader(namespace, loader=None, is_package=True)
            module = importlib.util.module_from_spec(spec)
            module.__file__ = str(path)
            module.__path__ = [str(directory)]
            sys.modules[namespace] = module
            exec(compile(base64.b64decode(source, validate=True), str(path), "exec"), module.__dict__)
            classes = {
                value for value in module.__dict__.values()
                if isinstance(value, type) and value is not BasePlugin
                and issubclass(value, BasePlugin)
                and value.__module__ in (namespace, name)
            }
            if len(classes) != 1:
                raise ValueError("В точке входа должен быть один класс BasePlugin.")
            instance = classes.pop()()
            instance._desktop_context = context
            self.call(installation_id, instance.on_plugin_load)
            self.loaded[installation_id] = (instance, context, namespace)
        except BaseException as error:
            context.active = False
            self.runtime.remove(installation_id)
            self.clear_modules(namespace)
            self.settings_of.pop(installation_id, None)
            message = str(error) or type(error).__name__
            if isinstance(error, ModuleNotFoundError) and (error.name or "").split(".")[0] in {
                "android", "java", "jnius", "chaquopy", "org",
            }:
                message = "Этот импорт относится к Android. Адаптируйте плагин для Desktop SDK."
            self.logger.exception("%s[%s]: %s", plugin_id, installation_id[:8], message)
            raise LoadError(message) from error

    @staticmethod
    def clear_modules(namespace):
        for name in tuple(sys.modules):
            if name == namespace or name.startswith(namespace + "."):
                sys.modules.pop(name, None)

    def unload(self, installation_id):
        if installation_id not in self.loaded:
            return
        instance, context, namespace = self.loaded.pop(installation_id)
        try:
            self.call(installation_id, instance.on_plugin_unload)
        except BaseException as error:
            self.restart_required = True
            self.report_error(installation_id, str(error) or type(error).__name__)
            raise RuntimeError("Выгрузка завершилась ошибкой. Перезапустите клиент.") from error
        finally:
            self.setting_actions = {key: value for key, value in self.setting_actions.items()
                                    if key[0] != installation_id}
            context.active = False
            self.runtime.remove(installation_id)
            self.clear_modules(namespace)
            self.settings_of.pop(installation_id, None)

    def settings(self, installation_id):
        instance, context, _ = self.loaded[installation_id]
        result = []
        keys = set()
        actions = {}
        for item in self.call(installation_id, instance.create_settings):
            if isinstance(item, Header):
                if not isinstance(item.text, str) or not item.text:
                    raise ValueError("Подраздел настроек требует непустой заголовок.")
                result.append({"kind": "header", "text": item.text})
                continue
            if (not isinstance(item, (Switch, Input, Selector, Button))
                or not isinstance(item.key, str) or not 0 < len(item.key) <= 128
                or not isinstance(item.text, str) or item.key in keys):
                raise ValueError("Некорректное описание настройки.")
            keys.add(item.key)
            value = None if isinstance(item, Button) else context.get(item.key, item.default)
            field = {"key": item.key, "text": item.text}
            if isinstance(item, Button):
                if not callable(item.on_click):
                    raise ValueError("Кнопка настроек требует обработчик нажатия.")
                previous = self.setting_actions.get((installation_id, item.key))
                token = previous["token"] if previous and previous["context"] is context else uuid.uuid4().hex
                actions[(installation_id, item.key)] = {"token": token, "context": context, "callback": item.on_click}
                field.update(kind="button", action_id=token)
                if isinstance(item, Link):
                    if not item.text.strip() or not isinstance(item.description, str):
                        raise ValueError("Ссылка на настройки требует непустое название и текстовое описание.")
                    field.update(kind="link", description=item.description)
            elif isinstance(item, Switch):
                field.update(kind="switch", value=bool(value))
            elif isinstance(item, Input):
                field.update(kind="input", value=str(value))
            else:
                if not item.items or not all(isinstance(text, str) for text in item.items):
                    raise ValueError("Selector требует непустой список строк.")
                index = value if type(value) is int and 0 <= value < len(item.items) else 0
                field.update(kind="selector", value=index, items=list(item.items))
            result.append(field)
        self.setting_actions = {key: value for key, value in self.setting_actions.items()
                                if key[0] != installation_id}
        self.setting_actions.update(actions)
        return result

    def settings_action(self, installation_id, action_id):
        action = next((entry for (identity, _), entry in self.setting_actions.items()
                       if identity == installation_id and entry["token"] == action_id), None)
        if action is None or not action["context"].active or installation_id not in self.loaded:
            raise ValueError("Кнопка настроек устарела или плагин выключен.")
        result = self.call(installation_id, action["callback"])
        if inspect.iscoroutine(result):
            self.runtime.schedule(action["context"], result)

    def shutdown(self):
        for installation_id in reversed(tuple(self.loaded)):
            try:
                self.unload(installation_id)
            except RuntimeError:
                pass
        self.runtime.stop()
