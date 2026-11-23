"""Named actions owned by one plugin installation."""
from dataclasses import dataclass
import re
import uuid

from . import ui
from ._runtime import context
from .models import Chat, Message


@dataclass(frozen=True)
class Invocation:
    source: str
    account_id: str = ""
    chat: Chat | None = None
    message: Message | None = None


class Action:
    def __init__(self, owner, name, title, callback):
        self._owner = owner
        self._id = uuid.uuid4().hex
        self._callback = callback
        self._bindings = []
        self._removed = False
        self.name = name
        self.title = title
        self.link = f"tg://ovyaction?installation={owner.installation_id}&name={name}"

    def __call__(self, invocation=None):
        if self._removed or not self._owner.active or context() is not self._owner:
            raise ui.ObjectDestroyed("Действие уже недоступно.")
        return self._callback(invocation or Invocation("button"))

    def _native(self, value=None):
        if value is None:
            return self(Invocation("shortcut"))
        return self(Invocation(value["source"], value.get("account_id", "")))

    async def configure_shortcut(self):
        if self._removed or context() is not self._owner:
            raise ui.ObjectDestroyed("Действие уже недоступно.")
        return await self._owner.manager.runtime.request("actions.configure", rule_id=self._id)

    def add_menu(self, menu, *, kinds=None):
        if self._removed or context() is not self._owner:
            raise ui.ObjectDestroyed("Действие уже недоступно.")

        def clicked(item):
            return self(Invocation(menu, item.account_id, item.chat, item.message))

        binding = ui.add_menu_item(menu, self.title, clicked, kinds=kinds)
        self._bindings.append(binding)
        return binding

    def remove(self):
        if self._removed:
            return
        self._removed = True
        runtime = self._owner.manager.runtime
        with runtime.lock:
            runtime.widget_rules.pop(self._id, None)
        for binding in self._bindings:
            binding.remove()
        self._bindings.clear()
        if self._owner.active:
            runtime.submit_ui(self._owner, "ui.action.remove", rule_id=self._id)


async def register(name, title, callback, *, tray=False, global_shortcut=False, link=False, shortcut=""):
    if not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9_.-]{0,47}", name):
        raise ValueError("Имя действия должно содержать 1–48 символов a-z, 0-9, _, . или -.")
    if not isinstance(title, str) or not title.strip() or len(title.encode("utf-16-le")) > 128:
        raise ValueError("Название действия должно содержать 1–64 символа UTF-16.")
    if not title.isprintable() or not callable(callback):
        raise ValueError("Укажите печатное название и обработчик действия.")
    if not isinstance(shortcut, str) or len(shortcut) > 80:
        raise ValueError("Некорректное сочетание клавиш.")
    owner = context()
    runtime = owner.manager.runtime
    action = Action(owner, name, title.strip(), callback)
    with runtime.lock:
        runtime.widget_rules[action._id] = (owner, action._native)
    try:
        await runtime.request("actions.register", rule_id=action._id, name=action.name,
                              title=action.title, tray=bool(tray), link=bool(link),
                              global_shortcut=bool(global_shortcut), shortcut=shortcut)
    except BaseException:
        action.remove()
        raise
    return action
