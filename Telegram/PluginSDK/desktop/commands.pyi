from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass
from typing import Any

from .models import Chat

REQUIRED: Any

@dataclass(frozen=True)
class Arg:
    name: str
    type: type = str
    choices: Sequence[Any] | None = None
    default: Any = REQUIRED
    rest: bool = False
    suggest: Callable[[str, SuggestContext], Any] | None = None
    description: str = ""

@dataclass(frozen=True)
class SuggestContext:
    account_id: str
    chat_id: str
    kind: str
    args: tuple[str, ...]

@dataclass(frozen=True)
class CommandContext:
    account_id: str
    chat: Chat | None
    args: tuple[Any, ...]
    async def reply(self, text: str) -> Any: ...
    def insert(self, text: str) -> None: ...

class Command:
    name: str
    def remove(self) -> None: ...

def add(name: str, handler: Callable[..., Any], *, description: str = "",
        args: Iterable[Arg] = (), aliases: Iterable[str] = (),
        kinds: Iterable[str] = ()) -> Command: ...
