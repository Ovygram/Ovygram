from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from typing import Any, Literal
from .models import Account, Chat
from .ui import WidgetRule

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

Predicate = Callable[[Result], bool | Awaitable[bool]]
Media = Literal["", "photo", "video", "document", "voice", "url"]

class Filter(WidgetRule):
    def __init__(self, target: str | dict[str, Any], account: Account | str, predicate: Predicate | None = None,
                 *, chat: Chat | None = None, media: Media = "", min_date: int = 0, max_date: int = 0) -> None: ...
    def more(self) -> None: ...

def add_filter(target: str | dict[str, Any], account: Account | str, predicate: Predicate | None = None,
               *, chat: Chat | None = None, media: Media = "", min_date: int = 0, max_date: int = 0) -> Filter: ...
