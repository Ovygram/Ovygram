"""Filters on the client's ordinary message search. SDK callbacks run in the plugin host."""

import asyncio
import inspect
from dataclasses import dataclass

from ._runtime import context
from .client import _account, _chat
from .ui import WidgetRule, _handle


@dataclass(frozen=True)
class Result:
    account_id: str
    id: int
    chat_id: int
    sender_id: int
    text: str
    date: int
    outgoing: bool
    has_media: bool
    links: int


class Filter(WidgetRule):
    def __init__(self, target, account, predicate=None, *, chat=None, media="", min_date=0, max_date=0):
        owner = context()
        account_id = _account(account)
        if chat is not None and _chat(chat).account_id != account_id:
            raise ValueError("Фильтр и чат должны принадлежать одному аккаунту.")
        if media not in ("", "photo", "video", "document", "voice", "url"):
            raise ValueError("Неизвестный тип медиа.")
        if any(type(date) is not int or not 0 <= date <= 2**31 - 1 for date in (min_date, max_date)):
            raise ValueError("Дата должна быть меткой Unix от 0 до 2147483647.")
        if max_date and max_date < min_date:
            raise ValueError("Конечная дата раньше начальной.")
        if predicate is not None and not callable(predicate):
            raise TypeError("Ожидается функция фильтра.")
        if predicate is None and not (min_date or max_date or media):
            raise ValueError("Укажите хотя бы одно условие поиска.")
        self._predicate = predicate
        self._account = account_id
        super().__init__(owner, "ui.search.add", self._event, target=_handle(target),
                         account_id=account_id, chat_id=str(chat.id) if chat is not None else "",
                         media=media, min_date=min_date, max_date=max_date,
                         local=predicate is not None)

    def _event(self, value):
        if value.get("closed"):
            with self.owner.manager.runtime.lock:
                self.owner.manager.runtime.widget_rules.pop(self.id, None)
                self.removed = True
            return None
        return self._page(value)

    async def _page(self, batch):
        async def evaluate():
            hidden = []
            for index, value in enumerate(batch["rows"]):
                row = Result(self._account, value["id"], int(value["chat_id"]), int(value["sender_id"]),
                             value["text"], value["date"], value["outgoing"], value["has_media"], value["links"])
                keep = self._predicate(row)
                if inspect.isawaitable(keep):
                    keep = await keep
                if type(keep) is not bool:
                    raise TypeError("Фильтр должен вернуть bool.")
                if not keep:
                    hidden.append(index)
            return hidden

        hidden = []
        try:
            hidden = await asyncio.wait_for(evaluate(), timeout=4)
        finally:
            if not self.removed and self.owner.active:
                self.owner.manager.runtime.submit_ui(
                    self.owner, "ui.search.result", rule_id=self.id, token=batch["token"], hidden=hidden)

    def more(self):
        """Continue from the raw server cursor after a run of six empty pages."""
        if not self.removed:
            self.owner.manager.runtime.submit_ui(self.owner, "ui.search.more", rule_id=self.id)


def add_filter(target, account, predicate=None, *, chat=None, media="", min_date=0, max_date=0):
    return Filter(target, account, predicate, chat=chat, media=media, min_date=min_date, max_date=max_date)
