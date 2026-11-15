"""Commands of a plugin for the message field, with suggestions.

    from desktop import commands

    commands.add("погода", self.weather, description="Погода в городе", args=[
        commands.Arg("город", suggest=self.cities),
        commands.Arg("дни", type=int, default=1, choices=[1, 3, 7]),
    ])

While the user types "/", Ovygram lists the commands, completes names and
arguments with Tab and shows what comes next. Enter runs the handler instead
of sending the text. A handler gets a CommandContext and the values of its
arguments; it may be a coroutine function.
"""

from dataclasses import dataclass, field

from ._runtime import context

REQUIRED = object()
_TYPES = {str: "str", int: "int", float: "float", bool: "bool"}


@dataclass(frozen=True)
class Arg:
    """An argument of a command.

    type is str, int, float or bool. choices lists the allowed values.
    An argument with a default may be left out; so may one with
    default=None. rest=True lets the last argument take the rest of the
    line with its spaces. suggest(prefix, context) returns the values to
    offer, strings or (value, description) pairs; it may be a coroutine
    function and gets a SuggestContext."""

    name: str
    type: type = str
    choices: object = None
    default: object = REQUIRED
    rest: bool = False
    suggest: object = None
    description: str = ""


@dataclass(frozen=True)
class SuggestContext:
    """Where suggestions are asked for: the account and chat of the field
    and the arguments already typed, as text."""

    account_id: str
    chat_id: str
    kind: str
    args: tuple = ()


@dataclass(frozen=True)
class CommandContext:
    """Where a command was run. chat is a Chat of desktop.client."""

    account_id: str
    chat: object = None
    args: tuple = field(default=(), repr=False)

    async def reply(self, text):
        """Sends text to the chat of the command."""
        from . import client
        return await client.send_message(self.chat, text)

    def insert(self, text):
        """Puts text into the message field."""
        from . import ui
        ui.insert_compose_text(text)


class Command:
    """A registered command; remove() takes it back, as unloading does."""

    def __init__(self, owner, name, handler, description, args, aliases, kinds):
        self.owner = owner
        self.name = name
        self.handler = handler
        self.description = description
        self.args = args
        self.aliases = aliases
        self.kinds = kinds

    def declaration(self):
        return {
            "name": self.name, "aliases": list(self.aliases), "kinds": list(self.kinds),
            "description": self.description,
            "args": [{
                "name": arg.name, "type": _TYPES[arg.type], "description": arg.description,
                "choices": list(arg.choices) if arg.choices is not None else None,
                "optional": arg.default is not REQUIRED,
                "default": None if arg.default is REQUIRED else arg.default,
                "rest": arg.rest, "suggest": arg.suggest is not None,
            } for arg in self.args],
        }

    def remove(self):
        if self.owner.active:
            self.owner.manager.runtime.remove_command(self)


def add(name, handler, *, description="", args=(), aliases=(), kinds=()):
    """Registers the command /name. kinds limits it to chats of the kinds
    "user", "chat" and "channel"; everywhere by default."""
    owner = context()
    args = tuple(args)
    if not callable(handler):
        raise TypeError("handler должен быть функцией.")
    for arg in args:
        if not isinstance(arg, Arg) or arg.type not in _TYPES:
            raise TypeError("Аргументы команды задаются объектами Arg с типом str, int, float или bool.")
        if arg.suggest is not None and not callable(arg.suggest):
            raise TypeError("suggest должен быть функцией.")
    command = Command(owner, str(name), handler, str(description), args, tuple(aliases), tuple(kinds))
    owner.manager.runtime.add_command(command)
    return command
