"""Plugin commands typed into the message field, on the side of the client.

Plugins declare commands through desktop.commands. Their declarations arrive
here from the plugin host and are checked as untrusted data. From the text of
the field the engine works out what the client draws: the list above the
field, the grey completion after the cursor and the line with the usage or
an error. It also decides whether a submitted text is a command, so the text
of a plugin command never goes to the chat.

Nothing here runs plugin code. Suggestions of a plugin are asked for through
the "request" of a view and come back later with suggestions().
"""

import math
import re
import threading

MAX_COMMANDS = 64
MAX_ARGS = 16
MAX_CHOICES = 100
MAX_ITEMS = 50
MAX_VALUE = 100
MAX_DESCRIPTION = 200
MAX_CACHE = 64
NAME = re.compile(r"[^\W\d][\w-]{0,31}\Z")
TYPES = ("str", "int", "float", "bool")
KINDS = ("user", "chat", "channel")
TRUE = ("true", "yes", "on", "1", "да")
FALSE = ("false", "no", "off", "0", "нет")
INACTIVE = {"active": False, "items": [], "selected": 0, "ghost": "", "footer": "", "error": False}


def _text(value, limit, name):
    if not isinstance(value, str) or len(value) > limit or any(ord(char) < 32 for char in value):
        raise ValueError(f"Некорректное поле {name} в описании команды.")
    return value


def _scalar(value, kind):
    """A choice or a default as the plugin declared it, of the type of its
    argument."""
    expected = {"str": str, "int": int, "float": (int, float), "bool": bool}[kind]
    if isinstance(value, bool) != (kind == "bool") or not isinstance(value, expected):
        raise ValueError("Значение аргумента не соответствует его типу.")
    if isinstance(value, float) and not math.isfinite(value):
        raise ValueError("Значение аргумента не является числом.")
    if isinstance(value, str):
        _text(value, MAX_VALUE, "choices")
    return value


def checked(commands):
    """The declarations of one plugin, normalized; ValueError otherwise."""
    if not isinstance(commands, list) or len(commands) > MAX_COMMANDS:
        raise ValueError("Некорректный список команд.")
    result = []
    names = set()
    for command in commands:
        if not isinstance(command, dict):
            raise ValueError("Некорректное описание команды.")
        aliases = command.get("aliases", [])
        if not isinstance(aliases, list) or len(aliases) > 8:
            raise ValueError("Некорректные псевдонимы команды.")
        titles = [command.get("name"), *aliases]
        for title in titles:
            if not isinstance(title, str) or not NAME.fullmatch(title) or title.casefold() in names:
                raise ValueError(f"Недопустимое или повторное имя команды: {title!r}.")
            names.add(title.casefold())
        kinds = command.get("kinds", [])
        if not isinstance(kinds, list) or any(kind not in KINDS for kind in kinds):
            raise ValueError("Некорректный список типов чатов команды.")
        declared = command.get("args", [])
        if not isinstance(declared, list) or len(declared) > MAX_ARGS:
            raise ValueError("Некорректный список аргументов команды.")
        args = []
        optional = False
        for index, arg in enumerate(declared):
            if not isinstance(arg, dict) or arg.get("type") not in TYPES:
                raise ValueError("Некорректное описание аргумента.")
            kind = arg["type"]
            choices = arg.get("choices")
            if choices is not None:
                if not isinstance(choices, list) or not 0 < len(choices) <= MAX_CHOICES:
                    raise ValueError("Некорректный список значений аргумента.")
                choices = [_scalar(choice, kind) for choice in choices]
            entry = {
                "name": _text(arg.get("name"), 32, "name"), "type": kind, "choices": choices,
                "description": _text(arg.get("description", ""), MAX_DESCRIPTION, "description"),
                "optional": bool(arg.get("optional")), "rest": bool(arg.get("rest")),
                "suggest": bool(arg.get("suggest")),
                "default": _scalar(arg["default"], kind) if arg.get("default") is not None else None,
            }
            if not entry["name"]:
                raise ValueError("У аргумента команды нет имени.")
            if optional and not entry["optional"]:
                raise ValueError("Обязательный аргумент не может идти после необязательного.")
            if entry["rest"] and (index != len(declared) - 1 or kind != "str"):
                raise ValueError("Остаток строки забирает только последний строковый аргумент.")
            optional = entry["optional"]
            args.append(entry)
        result.append({
            "name": titles[0], "aliases": aliases, "kinds": kinds, "args": args,
            "description": _text(command.get("description", ""), MAX_DESCRIPTION, "description"),
        })
    return result


def usage(args):
    return " ".join(("[{}]" if arg["optional"] else "<{}>").format(arg["name"]) for arg in args)


def split(text, rest_index=None):
    """(finished tokens, the token at the end, where it starts).

    A value with spaces or quotes is written in double quotes with backslash
    escapes. The argument number rest_index takes everything that is left,
    as it is typed."""
    done = []
    position = 0
    while True:
        while position < len(text) and text[position].isspace():
            position += 1
        start = position
        if rest_index is not None and len(done) == rest_index:
            return done, text[start:], start
        if text.startswith('"', position):
            position += 1
            chars = []
            closed = False
            while position < len(text):
                ch = text[position]
                if ch == "\\" and position + 1 < len(text) and text[position + 1] in ('"', "\\"):
                    chars.append(text[position + 1])
                    position += 2
                    continue
                if ch == '"':
                    position += 1
                    closed = True
                    break
                chars.append(ch)
                position += 1
            token = "".join(chars)
            if not closed or position >= len(text):
                return done, token, start
            done.append(token)
            continue
        while position < len(text) and not text[position].isspace():
            position += 1
        if position >= len(text):
            return done, text[start:], start
        done.append(text[start:position])


def convert(arg, raw):
    """The typed value of what the user wrote; ValueError with a message."""
    kind = arg["type"]
    try:
        if kind == "int":
            value = int(raw)
        elif kind == "float":
            value = float(raw.replace(",", "."))
            if not math.isfinite(value):
                raise ValueError
        elif kind == "bool":
            if raw.casefold() not in TRUE + FALSE:
                raise ValueError
            value = raw.casefold() in TRUE
        else:
            value = raw
    except ValueError:
        expected = {"int": "целое число", "float": "число", "bool": "true или false"}[kind]
        raise ValueError(f"{arg['name']}: ожидается {expected}.") from None
    if arg["choices"] is not None:
        for choice in arg["choices"]:
            if choice == value or (isinstance(choice, str) and choice.casefold() == raw.casefold()):
                return choice
        raise ValueError(f"{arg['name']}: допустимо {', '.join(map(str, arg['choices'][:8]))}.")
    return value


def quoted(value):
    if not value or any(char.isspace() or char in ('"', "\\") for char in value):
        escaped = value.replace("\\", "\\\\").replace('"', '\\"')
        return f'"{escaped}"'
    return value


class Engine:
    def __init__(self):
        self.lock = threading.RLock()
        self.tables = {}  # installation_id -> (plugin_id, commands)
        self.text = ""
        self.cursor = 0
        self.context = {}
        self.selected = 0
        self.dismissed = None  # The text Esc closed the panel for.
        self.cache = {}  # Suggestion key -> items of the plugin.
        self.token = 0
        self.pending = None  # (token, key) asked from a plugin.
        self.shown = None  # What the selection belongs to.

    # Declarations.

    def set(self, installation_id, plugin_id, commands):
        commands = checked(commands)
        with self.lock:
            if commands:
                self.tables[installation_id] = (plugin_id, commands)
            else:
                self.tables.pop(installation_id, None)
            self.forget()

    def remove(self, installation_id):
        with self.lock:
            self.tables.pop(installation_id, None)
            self.forget()

    def clear(self):
        with self.lock:
            self.tables.clear()
            self.forget()

    def forget(self):
        self.cache.clear()
        self.pending = None

    def titles(self, kind):
        """[(title, installation_id, plugin_id, command)] of the commands of
        a chat kind. A name several plugins use is written "plugin:name", or
        "plugin[id]:name" when two installations share plugin_id."""
        entries = []
        for installation_id, (plugin_id, commands) in self.tables.items():
            for command in commands:
                if not command["kinds"] or kind in command["kinds"]:
                    for name in (command["name"], *command["aliases"]):
                        entries.append((name, installation_id, plugin_id, command))
        count = {}
        pair_count = {}
        for name, _, plugin_id, _ in entries:
            key = name.casefold()
            count[key] = count.get(key, 0) + 1
            pair = (plugin_id.casefold(), key)
            pair_count[pair] = pair_count.get(pair, 0) + 1
        result = []
        for name, installation_id, plugin_id, command in entries:
            key = name.casefold()
            if count[key] == 1:
                title = name
            elif pair_count[(plugin_id.casefold(), key)] == 1:
                title = f"{plugin_id}:{name}"
            else:
                title = f"{plugin_id}[{installation_id[:8]}]:{name}"
            result.append((title, installation_id, plugin_id, command))
        return sorted(result, key=lambda entry: entry[0].casefold())

    def resolve(self, name, kind):
        for title, installation_id, plugin_id, command in self.titles(kind):
            if title.casefold() == name.casefold():
                return installation_id, plugin_id, command
        return None

    # The view.

    def input(self, text, cursor, context):
        with self.lock:
            self.text = text
            self.cursor = max(0, min(int(cursor), len(text)))
            self.context = dict(context)
            if self.dismissed != text:
                self.dismissed = None
            return self.view()

    def line(self):
        """The command text before the cursor, without "/", or None."""
        line = self.text[:self.cursor]
        if (not line.startswith("/") or line.startswith("//") or "\n" in self.text
                or self.dismissed == self.text or not self.tables):
            return None
        return line[1:]

    def state(self):
        """(items, completions, footer, error, request key, shown key).

        items are what the list shows, completions the texts Tab puts
        between the start of the current word and the cursor."""
        body = self.line()
        if body is None:
            return None
        kind = self.context.get("kind", "")
        if body[:1].isspace():
            return None
        if not any(char.isspace() for char in body):
            matches = [entry for entry in self.titles(kind)
                       if entry[0].casefold().startswith(body.casefold())][:MAX_ITEMS]
            if not matches:
                return None
            items = [{"title": "/" + title, "hint": usage(command["args"]),
                      "description": command["description"], "plugin": plugin_id}
                     for title, _, plugin_id, command in matches]
            return items, [(1, title + " ") for title, *_ in matches], "", False, None, ("name",)
        name, rest = body.split(None, 1) if body.split(None, 1)[1:] else (body.split()[0], "")
        offset = 1 + len(body) - len(rest)
        found = self.resolve(name, kind)
        if found is None:
            return None
        installation_id, plugin_id, command = found
        args = command["args"]
        rest_index = len(args) - 1 if args and args[-1]["rest"] else None
        done, current, start = split(rest, rest_index)
        footer = f"/{name} {usage(args)}".rstrip()
        if command["description"]:
            footer += " — " + command["description"]
        for arg, raw in zip(args, done):
            try:
                convert(arg, raw)
            except ValueError as error:
                return [], [], str(error), True, None, (installation_id, name, -1)
        index = len(done)
        if index >= len(args):
            if current or index > len(args):
                return [], [], "Лишний аргумент. " + footer, True, None, (installation_id, name, -1)
            return [], [], footer, False, None, (installation_id, name, index)
        arg = args[index]
        if arg["description"]:
            footer = f"{arg['name']}: {arg['description']}"
        request = None
        if arg["choices"] is not None:
            values = [{"value": str(choice), "description": ""} for choice in arg["choices"]]
        elif arg["type"] == "bool":
            values = [{"value": "true", "description": ""}, {"value": "false", "description": ""}]
        elif arg["suggest"]:
            key = (installation_id, command["name"], index, current, tuple(done))
            values = self.cache.get(key)
            if values is None:
                values, request = [], key
        else:
            values = []
        values = [value for value in values if value["value"].casefold().startswith(current.casefold())]
        error = False
        if current and not values and not arg["suggest"]:
            try:
                convert(arg, current)
            except ValueError as failure:
                footer, error = str(failure), True
        items = [{"title": value["value"], "hint": "", "description": value["description"],
                  "plugin": ""} for value in values[:MAX_ITEMS]]
        last = index == len(args) - 1
        completions = [(offset + start, (value["value"] if arg["rest"] else quoted(value["value"]))
                        + ("" if last else " ")) for value in values[:MAX_ITEMS]]
        return items, completions, footer, error, request, (installation_id, name, index)

    def view(self):
        state = self.state()
        if state is None:
            self.shown = None
            self.selected = 0
            return dict(INACTIVE)
        items, completions, footer, error, request, shown = state
        if shown != self.shown:
            self.shown = shown
            self.selected = 0
        self.selected = min(self.selected, max(len(items) - 1, 0))
        ghost = ""
        if completions:
            start, text = completions[self.selected]
            typed = self.text[start:self.cursor]
            # Only a continuation of exactly what is typed can follow it.
            if text.startswith(typed) and self.cursor == len(self.text):
                ghost = text[len(typed):].rstrip()
        elif shown[-1] != -1 and self.cursor == len(self.text) and self.text.endswith(" "):
            found = self.resolve(shown[1], self.context.get("kind", ""))
            if found is not None:
                ghost = usage(found[2]["args"][shown[2]:])
        result = {"active": True, "items": items, "selected": self.selected, "ghost": ghost,
                  "footer": footer, "error": error}
        if request is not None and (self.pending is None or self.pending[1] != request):
            self.token += 1
            self.pending = (self.token, request)
            installation_id, name, index, prefix, done = request
            result["request"] = {
                "token": self.token, "installation_id": installation_id, "name": name,
                "index": index, "prefix": prefix, "args": list(done),
                "context": {key: self.context.get(key, "") for key in ("kind", "account_id", "chat_id")}}
        return result

    def suggestions(self, token, items):
        """The answer of a plugin; True when the shown view changed."""
        if not isinstance(items, list):
            return False
        values = []
        for item in items[:MAX_ITEMS]:
            if (not isinstance(item, dict) or not isinstance(item.get("value"), str)
                    or not isinstance(item.get("description", ""), str)
                    or not 0 < len(item["value"]) <= MAX_VALUE):
                return False
            values.append({"value": item["value"],
                           "description": item.get("description", "")[:MAX_DESCRIPTION]})
        with self.lock:
            if self.pending is None or self.pending[0] != token:
                return False
            if len(self.cache) >= MAX_CACHE:
                self.cache.clear()
            self.cache[self.pending[1]] = values
            self.pending = None
            return True

    # Keys.

    def move(self, delta):
        with self.lock:
            state = self.state()
            if state is not None and state[0]:
                self.selected = (self.selected + delta) % len(state[0])
            return self.view()

    def complete(self, index=None):
        """Tab or a click: (new text, new cursor), or None when there is
        nothing to put into the field."""
        with self.lock:
            state = self.state()
            if state is None or not state[1]:
                return None
            index = self.selected if index is None else index
            if type(index) is not int or not 0 <= index < len(state[1]):
                return None
            start, text = state[1][index]
            result = self.text[:start] + text + self.text[self.cursor:]
            return result, start + len(text)

    def dismiss(self):
        with self.lock:
            self.dismissed = self.text
            return self.view()

    # Sending.

    def finish_name(self, text):
        """Enter on an unfinished command name completes it like Tab,
        instead of sending the fragment to the chat."""
        with self.lock:
            state = self.state() if text == self.text else None
            if state is None or state[5] != ("name",):
                return None
            return self.complete()

    def submit(self, text, kind):
        """None for a text that is no plugin command, {"error": message}
        for a command that can't run, otherwise what to run."""
        with self.lock:
            if (not text.startswith("/") or text.startswith("//") or "\n" in text
                    or not text[1:].strip() or text[1:2].isspace()):
                return None
            name, *rest = text[1:].split(None, 1)
            found = self.resolve(name, kind)
            if found is None:
                return None
            installation_id, _, command = found
            args = command["args"]
            rest_index = len(args) - 1 if args and args[-1]["rest"] else None
            source = rest[0].rstrip() if rest else ""
            done, current, start = split(source, rest_index)
            if current or source[start:].startswith('"'):
                done.append(current)
            if len(done) > len(args):
                return {"error": f"Лишний аргумент. /{name} {usage(args)}"}
            values = []
            for index, arg in enumerate(args):
                if index < len(done):
                    try:
                        values.append(convert(arg, done[index]))
                    except ValueError as error:
                        return {"error": str(error)}
                elif arg["optional"]:
                    values.append(arg["default"])
                else:
                    return {"error": f"Не хватает аргумента. /{name} {usage(args)}"}
            return {"installation_id": installation_id, "name": command["name"], "values": values}
