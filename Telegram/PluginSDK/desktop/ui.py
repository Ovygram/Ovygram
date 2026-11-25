"""Plugin dialogs, windows and widgets described by a tree of nodes.

Every call only queues a command for the client, so it can be made from
on_plugin_load, a hook or a background task. The client builds the widgets
on the main thread, and widget callbacks always run on the main thread.
"""

import re
import uuid
from dataclasses import dataclass

from ._runtime import context


class ObjectDestroyed(RuntimeError):
    """The dialog or window was closed, or its plugin was unloaded."""


def _identity():
    return uuid.uuid4().hex


def _layout(width=None, stretch=None):
    """Size of a node inside a Row: a fixed width in pixels, "fit" for its
    natural width, or a share of the remaining width (stretch, default 1)."""
    result = {}
    if width is not None:
        if width != "fit" and (type(width) is not int or width < 0):
            raise ValueError('width должен быть числом пикселей или "fit".')
        result["width"] = width
    if stretch is not None:
        if type(stretch) is not int or not 0 <= stretch <= 100:
            raise ValueError("stretch должен быть целым от 0 до 100.")
        result["stretch"] = stretch
    return result


def _choice(value, allowed, name):
    if value not in allowed:
        raise ValueError(f"{name} должен быть одним из: {', '.join(allowed)}.")
    return value


def _hex_color(value):
    if not isinstance(value, str) or not re.fullmatch(r"#[0-9A-Fa-f]{6}", value):
        raise ValueError("Цвет задаётся в формате #RRGGBB.")
    return value.upper()


def _button_colors(colors):
    if colors is None:
        return {}
    if not isinstance(colors, dict) or colors.keys() - {"text", "background", "border"}:
        raise ValueError("Цвета кнопки задаются словарём с ключами text, background и border.")
    return {role: _hex_color(color) for role, color in colors.items()}


def _slider_value(value, minimum, maximum):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError("Значение ползунка должно быть целым и находиться в его диапазоне.")
    return value


class Node:
    type = ""

    def __init__(self, width=None, stretch=None, **props):
        self.id = _identity()
        self.props = {**props, **_layout(width, stretch)}
        self.children = ()
        self.handlers = {}

    def to_json(self):
        result = {"type": self.type, "id": self.id, **self.props}
        if self.children:
            result["children"] = [child.to_json() for child in self.children]
        return result

    def walk(self):
        yield self
        for child in self.children:
            yield from child.walk()


class Column(Node):
    type = "column"

    def __init__(self, *children, width=None, stretch=None, gap=None, visible=None):
        if gap is not None and (type(gap) is not int or not 0 <= gap <= 48):
            raise ValueError("gap должен быть целым от 0 до 48.")
        super().__init__(width, stretch, **({"gap": gap} if gap is not None else {}))
        if visible is not None:
            self.props["visible"] = bool(visible)
        self.children = tuple(_node(child) for child in children)


class Card(Column):
    """A padded group using the client's current palette."""
    type = "card"

    def __init__(self, *children, width=None, stretch=None, gap=None, visible=None, background=None):
        super().__init__(*children, width=width, stretch=stretch, gap=gap, visible=visible)
        if background is not None:
            self.props["background"] = _hex_color(background)


class Progress(Node):
    """A thin meter. Update its value in the range from zero to one."""
    type = "progress"

    def __init__(self, value=0.0):
        if not 0 <= value <= 1:
            raise ValueError("Значение шкалы должно быть от 0 до 1.")
        super().__init__(value=float(value))


class Divider(Node):
    """A thin separator using the current theme."""
    type = "divider"


class Row(Node):
    """Children side by side. align is the vertical alignment of children:
    "top", "center" or "bottom"; spacing is the gap between them in pixels."""

    type = "row"

    def __init__(self, *children, align="center", spacing=8, width=None, stretch=None, wrap=False):
        if type(spacing) is not int or not 0 <= spacing <= 200:
            raise ValueError("spacing должен быть целым от 0 до 200.")
        super().__init__(width, stretch, align=_choice(align, ("top", "center", "bottom"), "align"),
                         spacing=spacing, **({"wrap": True} if wrap else {}))
        self.children = tuple(_node(child) for child in children)


class Text(Node):
    """align is the horizontal alignment: "left", "center" or "right"."""

    type = "text"

    def __init__(self, text, align="left", width=None, stretch=None, *, tone="normal", bold=False, color=None):
        super().__init__(width, stretch, text=str(text),
                         align=_choice(align, ("left", "center", "right"), "align"),
                         tone=_choice(tone, ("normal", "muted", "accent", "positive", "attention"), "tone"),
                         bold=bool(bold))
        if color is not None:
            self.props["color"] = _hex_color(color)


class Button(Node):
    type = "button"

    def __init__(self, text, on_click=None, width=None, stretch=None, *, enabled=True, tooltip="", variant="primary",
                 visible=None, detail="", selected=False, palette="theme", colors=None):
        super().__init__(width, stretch, text=str(text), enabled=bool(enabled), tooltip=str(tooltip),
                         variant=_choice(variant, ("primary", "secondary", "danger", "trash", "chip", "code",
                                                   "link", "choice"), "variant"))
        if visible is not None:
            self.props["visible"] = bool(visible)
        if variant == "choice":
            self.props.update(detail=str(detail), selected=bool(selected))
        if _choice(palette, ("theme", "ovygram"), "palette") == "ovygram":
            self.props["palette"] = palette
        if colors is not None:
            self.props["colors"] = _button_colors(colors)
        if on_click is not None:
            self.handlers["click"] = on_click


class Input(Node):
    type = "input"

    def __init__(self, text="", placeholder="", on_change=None, width=None, stretch=None, *, search=False):
        super().__init__(width, stretch, text=str(text), placeholder=str(placeholder))
        if search:
            self.props["search"] = True
        if on_change is not None:
            self.handlers["change"] = on_change


class Switch(Node):
    type = "switch"

    def __init__(self, text, value=False, on_toggle=None, width=None, stretch=None):
        super().__init__(width, stretch, text=str(text), value=bool(value))
        if on_toggle is not None:
            self.handlers["toggle"] = on_toggle


class Slider(Node):
    type = "slider"

    def __init__(self, value=0, on_change=None, width=None, stretch=None, *, minimum=0, maximum=255, label=""):
        if type(minimum) is not int or type(maximum) is not int or not -1_000_000 <= minimum < maximum <= 1_000_000:
            raise ValueError("Границы ползунка должны быть целыми от −1 000 000 до 1 000 000.")
        super().__init__(width, stretch, value=_slider_value(value, minimum, maximum),
                         minimum=minimum, maximum=maximum, label=str(label))
        if on_change is not None:
            self.handlers["slide"] = on_change


class Image(Node):
    type = "image"

    def __init__(self, path, height=0, width=None, stretch=None):
        super().__init__(width, stretch, path=str(path), height=int(height))


class Spacer(Node):
    type = "spacer"

    def __init__(self, height, width=None, stretch=None):
        super().__init__(width, stretch, height=int(height))


class Canvas(Node):
    """Custom drawing: a list of draw commands, see the draw helpers below.

    Negative x or y count from the right or bottom edge, as in decorations."""

    type = "canvas"

    def __init__(self, height, commands=(), on_click=None, width=None, stretch=None):
        super().__init__(width, stretch, height=int(height), commands=list(commands))
        if on_click is not None:
            self.handlers["click"] = on_click


def fill_rect(x, y, width, height, color, radius=0):
    return {"op": "rect", "x": x, "y": y, "width": width, "height": height,
            "color": color, "radius": radius}


def line(x1, y1, x2, y2, color, width=1):
    return {"op": "line", "x1": x1, "y1": y1, "x2": x2, "y2": y2, "color": color, "width": width}


def ellipse(x, y, width, height, color):
    return {"op": "ellipse", "x": x, "y": y, "width": width, "height": height, "color": color}


def text(x, y, value, color, size=13):
    return {"op": "text", "x": x, "y": y, "text": str(value), "color": color, "size": size}


def _node(value):
    if isinstance(value, Node):
        return value
    if isinstance(value, str):
        return Text(value)
    raise TypeError("Элемент интерфейса должен быть Node или строкой.")


class Surface:
    """A shown dialog or window that belongs to the calling plugin."""

    kind = ""

    def __init__(self, content, title="", width=0, height=0, on_close=None):
        self.owner = context()
        self.id = _identity()
        self.content = _node(content)
        self.title = str(title)
        self.width = int(width)
        self.height = int(height)
        self.on_close = on_close
        self.closed = False
        self.nodes = {node.id: node for node in self.content.walk()}

    def show(self):
        runtime = self.owner.manager.runtime
        runtime.add_surface(self)
        runtime.submit_ui(self.owner, "ui.show", object_id=self.id, kind=self.kind,
                          title=self.title, width=self.width, height=self.height,
                          tree=self.content.to_json(), **self._placement())
        return self

    def _placement(self):
        return {}

    def update(self, node, **props):
        self._check(node)
        if isinstance(node, Button) and "colors" in props:
            props["colors"] = _button_colors(props["colors"])
        if isinstance(node, Text) and props.get("color") is not None:
            props["color"] = _hex_color(props["color"])
        if isinstance(node, Card) and props.get("background") is not None:
            props["background"] = _hex_color(props["background"])
        if isinstance(node, Slider) and "value" in props:
            _slider_value(props["value"], node.props["minimum"], node.props["maximum"])
        node.props.update(props)
        self.owner.manager.runtime.submit_ui(
            self.owner, "ui.update", object_id=self.id, node_id=node.id, props=props)

    def draw(self, canvas, commands):
        if not isinstance(canvas, Canvas):
            raise TypeError("draw принимает Canvas.")
        self.update(canvas, commands=list(commands))

    def close(self):
        if self.closed:
            return
        self.owner.manager.runtime.submit_ui(self.owner, "ui.close", object_id=self.id)

    def _check(self, node):
        if self.closed:
            raise ObjectDestroyed("Окно уже закрыто.")
        if self.nodes.get(getattr(node, "id", None)) is not node:
            raise ValueError("Элемент не принадлежит этому окну.")


class Dialog(Surface):
    """A modal box in the active Telegram window."""

    kind = "dialog"

    def __init__(self, content, title="", width=0, height=0, on_close=None, *, footer_action=None):
        super().__init__(content, title, width, height, on_close)
        self.footer_action = footer_action
        if footer_action is not None:
            if not isinstance(footer_action, Button) or footer_action.id in self.nodes:
                raise ValueError("Действие внизу диалога должно быть отдельной кнопкой.")
            self.nodes[footer_action.id] = footer_action

    def _placement(self):
        if self.footer_action is None:
            return {}
        return {"footer_action": self.footer_action.to_json()}


class Window(Surface):
    """A separate floating window."""

    kind = "window"

    def __init__(self, content, title="", width=420, height=480, on_close=None, *, max_content_width=None):
        super().__init__(content, title, width, height, on_close)
        self.max_content_width = max_content_width

    def _placement(self):
        return {} if self.max_content_width is None else {"max_content_width": int(self.max_content_width)}


class ChatBar(Surface):
    """A bar above the messages of one chat, below Telegram's own bars.

    The bar is shown while that chat is open in a window and stays until
    close() or until the plugin is unloaded.
    """

    kind = "chat_bar"

    def __init__(self, chat, content, on_close=None):
        account_id = getattr(chat, "account_id", None)
        chat_id = getattr(chat, "id", None)
        if not account_id or chat_id is None:
            raise TypeError("ChatBar принимает Chat из desktop.client.")
        super().__init__(content, on_close=on_close)
        self.account_id = str(account_id)
        self.chat_id = str(chat_id)

    def _placement(self):
        return {"account_id": self.account_id, "chat_id": self.chat_id}


class Section(Surface):
    """A page in the main column of the active window, in place of a chat.

    It has a back button and the title on top, and the content scrolls.
    Back and the window history work as for Telegram's own pages: the user
    can open a chat and come back. The section is closed by close(), when
    the window history drops it (on_close is called then) or on unload.
    """

    kind = "section"

    def __init__(self, content, title="", on_close=None, *, max_content_width=None):
        super().__init__(content, title, on_close=on_close)
        self.max_content_width = max_content_width

    def _placement(self):
        return {} if self.max_content_width is None else {"max_content_width": int(self.max_content_width)}


# Built-in Telegram widgets a plugin can change. The names stay the same
# across Telegram updates; each one may exist once per open window.
ELEMENTS = (
    "chat.top_bar.call",
    "chat.top_bar.search",
    "chat.top_bar.info",
    "chat.top_bar.menu",
    # Hiding compose.attach leaves a gap: Telegram reserves its width.
    "compose.attach",
    "compose.emoji",
    "compose.send",
    "compose.field",
)


class Element:
    """A built-in Telegram widget. Changes belong to the calling plugin and
    are undone when it is unloaded."""

    def __init__(self, name):
        if name not in ELEMENTS:
            raise ValueError(f"Неизвестный элемент {name!r}. Доступны: {', '.join(ELEMENTS)}.")
        self.owner = context()
        self.name = name

    def hide(self):
        self.owner.manager.runtime.submit_ui(self.owner, "ui.element.hide", element=self.name, hidden=True)

    def show(self):
        """Removes this plugin's hide rule; Telegram decides the visibility again."""
        self.owner.manager.runtime.submit_ui(self.owner, "ui.element.hide", element=self.name, hidden=False)

    def on_click(self, callback, replace=False):
        """Calls callback() on click. With replace=True Telegram's own action
        is not run."""
        if not callable(callback):
            raise TypeError("callback должен быть функцией.")
        runtime = self.owner.manager.runtime
        with runtime.lock:
            runtime.element_handlers[(self.owner.installation_id, self.name)] = callback
        runtime.submit_ui(self.owner, "ui.element.click", element=self.name,
                          enabled=True, replace=bool(replace))

    def off_click(self):
        runtime = self.owner.manager.runtime
        with runtime.lock:
            runtime.element_handlers.pop((self.owner.installation_id, self.name), None)
        runtime.submit_ui(self.owner, "ui.element.click", element=self.name, enabled=False)


def element(name):
    return Element(name)


def set_colors(colors):
    """Changes palette colors, e.g. {"windowBg": "#101820"}. Names are the
    keys of a Telegram .tdesktop-theme. Undone by reset_colors() or unload."""
    if not isinstance(colors, dict) or not colors:
        raise ValueError("Ожидается непустой словарь цветов.")
    for key, value in colors.items():
        if not isinstance(key, str) or not isinstance(value, str) or not value.startswith("#"):
            raise ValueError("Цвет задаётся строкой #rrggbb или #aarrggbb.")
    owner = context()
    owner.manager.runtime.submit_ui(owner, "ui.palette", colors=dict(colors))


def reset_colors():
    owner = context()
    owner.manager.runtime.submit_ui(owner, "ui.palette", reset=True)


class Insert(Surface):
    """Mount a UI tree inside a layout or before/after a row from widget_tree()."""

    kind = "insert"

    def __init__(self, target, content, *, position="inside", on_close=None):
        super().__init__(content, on_close=on_close)
        _choice(position, ("inside", "before", "after"), "position")
        self.target = _handle(target)
        self.position = position

    def _placement(self):
        return {"target": self.target, "position": self.position}


class Overlay(Surface):
    """A Canvas drawn over any widget from widget_tree(); it follows the
    widget's size. Without on_click on the canvas, clicks pass through."""

    kind = "overlay"

    def __init__(self, target, canvas, on_close=None):
        if not isinstance(canvas, Canvas):
            raise TypeError("Overlay принимает Canvas.")
        super().__init__(canvas, on_close=on_close)
        self.target = _handle(target)

    def _placement(self):
        return {"target": self.target, "clickable": "click" in self.content.handlers}


# Generic access to any Telegram widget. Widgets are found by a selector,
# so a rule also applies to widgets created later, e.g. in a new chat:
#   cls       class name from widget_tree(), e.g. "Ui::IconButton"
#   ancestor  class name of any parent, e.g. "HistoryView::TopBarWidget"
#   name      Qt objectName
#   accessible accessible name (localized, as shown by widget_tree())
#   element   a name from ELEMENTS
#   index     position among matching siblings, from 0
_SELECTOR_KEYS = ("cls", "ancestor", "name", "accessible", "element", "index")


def _selector(selector):
    unknown = set(selector) - set(_SELECTOR_KEYS)
    if unknown:
        raise ValueError(f"Неизвестные поля селектора: {', '.join(sorted(unknown))}.")
    if not any(selector.get(key) for key in ("cls", "name", "accessible", "element")):
        raise ValueError("Селектору нужен cls, name, accessible или element.")
    if "index" in selector and (type(selector["index"]) is not int or selector["index"] < 0):
        raise ValueError("index должен быть неотрицательным целым.")
    return {key: value for key, value in selector.items() if value is not None}


def _handle(value):
    handle = value.get("handle") if isinstance(value, dict) else value
    if not isinstance(handle, str) or not handle.isdigit():
        raise ValueError("Ожидается узел или handle из widget_tree().")
    return handle


class WidgetRule:
    """A rule owned by the calling plugin; undone by remove() or unload."""

    def __init__(self, owner, method, callback=None, **parameters):
        self.owner = owner
        self.id = _identity()
        self.method = method
        self.removed = False
        runtime = owner.manager.runtime
        if callback is not None:
            with runtime.lock:
                runtime.widget_rules[self.id] = (owner, callback)
        runtime.submit_ui(owner, method, rule_id=self.id, **parameters)

    def remove(self):
        runtime = self.owner.manager.runtime
        with runtime.lock:
            if runtime.widget_rules.pop(self.id, None) is None and self.removed:
                return
        self.removed = True
        if not self.owner.active:
            return
        if self.method == "ui.compose.watch":
            runtime.submit_ui(self.owner, "ui.compose.watch", rule_id=self.id, enabled=False)
        elif self.method == "ui.notification.add":
            runtime.submit_ui(self.owner, "ui.notification.remove", rule_id=self.id)
        elif self.method == "ui.menu.add":
            runtime.submit_ui(self.owner, "ui.menu.remove", rule_id=self.id)
        elif self.method == "ui.list_visibility.add":
            runtime.submit_ui(self.owner, "ui.list_visibility.remove", rule_id=self.id)
        elif self.method == "ui.search.add":
            runtime.submit_ui(self.owner, "ui.search.remove", rule_id=self.id)
        elif self.method == "ui.panel.add":
            runtime.submit_ui(self.owner, "ui.panel.remove", rule_id=self.id)
        elif self.method.startswith("ui.native."):
            runtime.submit_ui(self.owner, "ui.native.disconnect", rule_id=self.id)
        else:
            runtime.submit_ui(self.owner, "ui.widgets.unrule", rule_id=self.id)


class Placement(WidgetRule):
    """Keep one independent surface per live target matching the selector."""

    def __init__(self, factory, position="inside", **selector):
        if not callable(factory):
            raise TypeError("factory должна создавать содержимое для указанного элемента.")
        _choice(position, ("inside", "before", "after"), "position")
        self._mounted = {}

        def available(handle):
            if self.removed or (handle in self._mounted and not self._mounted[handle].closed):
                return
            surface = Insert(handle, factory(handle), position=position,
                             on_close=lambda: self._mounted.pop(handle, None))
            self._mounted[handle] = surface
            surface.show()

        super().__init__(context(), "ui.widgets.rule", available,
                         action="available", position=position, selector=_selector(selector))

    def remove(self):
        super().remove()
        for surface in tuple(self._mounted.values()):
            if not surface.closed:
                surface.close()
        self._mounted.clear()


async def widget_tree(depth=8, root=None):
    """The widget tree of the active window as nested dicts with handle,
    cls, visible, geometry, and name, accessible, element and children
    when present. Handles stay valid while the widget exists."""
    owner = context()
    return await owner.manager.runtime.request(
        "widgets.tree", depth=int(depth), root=_handle(root) if root is not None else None)


async def widget_at_cursor():
    """Describe the visible widget under the pointer for the placement inspector."""
    return await context().manager.runtime.request("widgets.at_cursor")


async def get_property(widget, name):
    """Reads a Qt property, e.g. "geometry", "visible", "toolTip"."""
    owner = context()
    return await owner.manager.runtime.request("widgets.get", handle=_handle(widget), property=str(name))


def set_property(widget, name, value):
    """Writes a Qt property. Telegram may overwrite it on its next layout."""
    owner = context()
    owner.manager.runtime.submit_ui(owner, "ui.widgets.set", handle=_handle(widget),
                                    property=str(name), value=value)


def hide_widgets(**selector):
    """Hides matching widgets now and whenever Telegram shows them again.
    The parent is not relaid out, so a gap may remain; use element() for
    the named elements, which close the gap."""
    return WidgetRule(context(), "ui.widgets.rule", action="hide", selector=_selector(selector))


def on_widget_click(callback, replace=False, **selector):
    if not callable(callback):
        raise TypeError("callback должен быть функцией.")
    return WidgetRule(context(), "ui.widgets.rule", callback, action="click",
                      replace=bool(replace), selector=_selector(selector))


def on_key(sequence, callback, replace=False, element=None):
    """Calls callback() on a key combination like "Ctrl+Alt+K". With
    replace=True Telegram doesn't receive the key, and the rule also wins
    over Telegram's own shortcuts. With element, e.g. "compose.field", the
    rule works only while the focus is inside that element. Keys taken by
    the system or the input method (e.g. Ctrl+Shift+U in IBus) never reach
    Telegram."""
    if not isinstance(sequence, str) or not sequence:
        raise ValueError("Ожидается сочетание клавиш, например Ctrl+Alt+K.")
    if not callable(callback):
        raise TypeError("callback должен быть функцией.")
    if element is not None and element not in ELEMENTS:
        raise ValueError(f"Неизвестный элемент {element!r}.")
    return WidgetRule(context(), "ui.keys.add", callback, sequence=sequence,
                      replace=bool(replace), element=element or "")


def _footer_height(value):
    if type(value) is not int or not 0 <= value <= 256:
        raise ValueError("footer_height: целое число от 0 до 256.")
    return value


class ChatDecoration:
    """Draw commands painted over a chat's row in the chat list.

    Coordinates are relative to the row; negative x or y count from its
    right or bottom edge, e.g. fill_rect(-28, 8, 20, 20, ...) is a square
    near the right edge. background tints the whole row, except when the
    chat is open. Everything is painted over the row, so use a translucent
    #aarrggbb background: an opaque one hides the row's text. Undone by
    remove() or when the plugin is unloaded."""

    def __init__(self, owner, account_id, chat_id, commands, background, footer_height=0):
        self.owner = owner
        self.id = _identity()
        self.account_id = account_id
        self.chat_id = chat_id
        self.footer_height = _footer_height(footer_height)
        self.update(commands, background)

    def update(self, commands=(), background=None):
        if background is not None and (not isinstance(background, str) or not background.startswith("#")):
            raise ValueError("background задаётся строкой #rrggbb или #aarrggbb.")
        self.owner.manager.runtime.submit_ui(
            self.owner, "ui.decoration.set", decoration_id=self.id, account_id=self.account_id,
            chat_id=self.chat_id, commands=list(commands), background=background,
            footer_height=self.footer_height)

    def remove(self):
        if self.owner.active:
            self.owner.manager.runtime.submit_ui(self.owner, "ui.decoration.remove", decoration_id=self.id)


def decorate_chat(chat, commands=(), background=None, account=None, *, footer_height=0):
    """Decorates one chat's row, or every chat of an account when chat is
    "*" and account is given (an Account or account ID)."""
    if chat == "*":
        if footer_height:
            raise ValueError("Высота задаётся для конкретного чата.")
        account_id = getattr(account, "id", account)
        if not isinstance(account_id, str) or not account_id:
            raise ValueError('Для chat="*" укажи account.')
        return ChatDecoration(context(), account_id, "*", commands, background, footer_height)
    account_id = getattr(chat, "account_id", None)
    chat_id = getattr(chat, "id", None)
    if not account_id or chat_id is None:
        raise TypeError('decorate_chat принимает Chat из desktop.client или "*".')
    return ChatDecoration(context(), str(account_id), str(chat_id), commands, background, footer_height)


def on_compose_change(callback):
    """Calls callback(state) when the user edits the message field. state
    is a dict with text, cursor, selection_start and selection_end; the
    positions index state["text"]. Returns a rule with remove()."""
    if not callable(callback):
        raise TypeError("callback должен быть функцией.")
    return WidgetRule(context(), "ui.compose.watch", callback, enabled=True)


async def editor_snapshot(chat, *, kind="message"):
    """Read the visible draft of chat, or its open attachment preview with kind='album'."""
    _choice(kind, ("message", "album"), "kind")
    return await context().manager.runtime.request(
        "compose.snapshot", str(chat.account_id), chat_id=str(chat.id), kind=kind)


async def _editor_action(snapshot, op, **params):
    if not isinstance(snapshot, dict) or not all(
            snapshot.get(key) for key in ("field", "account_id", "chat_id", "kind")):
        raise ValueError("Нужен актуальный снимок редактора.")
    return await context().manager.runtime.request(
        "compose.apply", snapshot["account_id"], chat_id=snapshot["chat_id"],
        kind=snapshot["kind"], expected=snapshot, op=op, **params)


async def set_draft(snapshot, text, tags=()):
    """Set the draft when its snapshot still matches. Tag ranges are in UTF-16 units."""
    if not isinstance(text, str):
        raise TypeError("Текст черновика должен быть строкой.")
    return await _editor_action(snapshot, "text", text=text, tags=list(tags))


async def suggest_draft(snapshot, text):
    """Replace and select text at the current selection of a focused editor."""
    if not isinstance(text, str):
        raise TypeError("Текст подсказки должен быть строкой.")
    return await _editor_action(snapshot, "suggestion", text=text)


async def editor_reply(snapshot, message_id=None):
    if message_id is None:
        return await _editor_action(snapshot, "clear_reply")
    if type(message_id) is not int or message_id <= 0:
        raise ValueError("Некорректный ID сообщения.")
    return await _editor_action(snapshot, "reply", message_id=message_id)


async def editor_edit(snapshot, message_id):
    if type(message_id) is not int or message_id <= 0:
        raise ValueError("Некорректный ID сообщения.")
    return await _editor_action(snapshot, "edit", message_id=message_id)


async def choose_attachments(snapshot):
    """Open the native file chooser for the snapshot's editor."""
    return await _editor_action(snapshot, "attachments")


async def reorder_attachments(snapshot, order):
    order = list(order)
    if (snapshot.get("kind") != "album" or not all(type(i) is int for i in order)
            or sorted(order) != list(range(len(snapshot["files"])))):
        raise ValueError("Нужна перестановка всех индексов альбома.")
    return await _editor_action(snapshot, "album_order", order=order)


async def set_attachment_caption(snapshot, index, caption):
    if (snapshot.get("kind") != "album" or type(index) is not int
            or not 0 <= index < len(snapshot["files"]) or not isinstance(caption, str)):
        raise ValueError("Нужны индекс вложения и подпись.")
    return await _editor_action(snapshot, "album_caption", index=index, caption=caption)


async def compose_state():
    """The same state dict for the message field in the active window, or None."""
    owner = context()
    return await owner.manager.runtime.request("compose.get_state")


def replace_compose_text(start, end, text, select=False):
    """Replaces text between positions start and end of the message field,
    keeping the formatting of the rest. With select=True the inserted text
    stays selected, like an inline suggestion."""
    if type(start) is not int or type(end) is not int or not 0 <= start <= end:
        raise ValueError("Ожидаются позиции 0 <= start <= end.")
    owner = context()
    owner.manager.runtime.submit_ui(owner, "ui.compose.replace", start=start, end=end,
                                    text=str(text), select=bool(select))


class MessageDecoration:
    """Draw commands painted over a message bubble, in bubble coordinates;
    negative x or y count from the bubble's right or bottom edge.
    background tints the bubble. Undone by remove() or unload."""

    def __init__(self, owner, message, commands, background, footer_height=0):
        self.owner = owner
        self.id = _identity()
        self.account_id = str(message.account_id)
        self.chat_id = str(message.chat_id)
        self.message_id = int(message.id)
        self.footer_height = _footer_height(footer_height)
        self.update(commands, background)

    def update(self, commands=(), background=None):
        if background is not None and (not isinstance(background, str) or not background.startswith("#")):
            raise ValueError("background задаётся строкой #rrggbb или #aarrggbb.")
        self.owner.manager.runtime.submit_ui(
            self.owner, "ui.message_decoration.set", decoration_id=self.id,
            account_id=self.account_id, chat_id=self.chat_id, message_id=self.message_id,
            commands=list(commands), background=background,
            footer_height=self.footer_height)

    def remove(self):
        if self.owner.active:
            self.owner.manager.runtime.submit_ui(
                self.owner, "ui.message_decoration.remove", decoration_id=self.id)


def decorate_message(message, commands=(), background=None, *, footer_height=0):
    """Decorates a Message from desktop.client or an event.

    Painted over the bubble, so use a translucent #aarrggbb background: an
    opaque one hides the message text."""
    if not all(hasattr(message, key) for key in ("account_id", "chat_id", "id")):
        raise TypeError("decorate_message принимает Message из desktop.client.")
    return MessageDecoration(context(), message, commands, background, footer_height)


class HiddenChats:
    """Chats hidden from the chat list and its folders by the calling
    plugin. Search still finds them. Undone by remove() or unload."""

    def __init__(self, owner, chats):
        self.owner = owner
        self.id = _identity()
        self.accounts = {}
        for chat in chats:
            account_id = getattr(chat, "account_id", None)
            chat_id = getattr(chat, "id", None)
            if not account_id or chat_id is None:
                raise TypeError("hide_chats принимает Chat из desktop.client.")
            self.accounts.setdefault(str(account_id), set()).add(str(chat_id))
        if not self.accounts:
            raise ValueError("Укажи хотя бы один чат.")
        for account_id, chat_ids in self.accounts.items():
            owner.manager.runtime.submit_ui(
                owner, "ui.chat_list.hide", rule_id=f"{self.id}:{account_id}",
                account_id=account_id, chat_ids=sorted(chat_ids))

    def remove(self):
        if not self.owner.active:
            return
        for account_id in self.accounts:
            self.owner.manager.runtime.submit_ui(
                self.owner, "ui.chat_list.unhide", rule_id=f"{self.id}:{account_id}")
        self.accounts = {}


def hide_chats(chats):
    """Hides chats (Chat objects, possibly from several accounts)."""
    return HiddenChats(context(), list(chats))


async def compose_text():
    """Text of the message field in the active window, or None."""
    owner = context()
    return await owner.manager.runtime.request("compose.get_text")


def set_compose_text(text):
    """Replaces the text of the message field in the active window."""
    owner = context()
    owner.manager.runtime.submit_ui(owner, "ui.compose.set", text=str(text))


def insert_compose_text(text):
    """Inserts text at the cursor of the message field in the active window."""
    owner = context()
    owner.manager.runtime.submit_ui(owner, "ui.compose.insert", text=str(text))


def set_clipboard(text):
    """Puts text into the system clipboard."""
    owner = context()
    owner.manager.runtime.submit_ui(owner, "ui.clipboard", text=str(text))


def show_dialog(content, title="", width=0, on_close=None, *, footer_action=None):
    return Dialog(content, title, width, on_close=on_close, footer_action=footer_action).show()


@dataclass(frozen=True)
class MenuContext:
    """Where a plugin menu item was clicked.

    account_id is the account of the window. chat is the chat of a "chat" or
    "message" menu, message the message of a "message" menu, otherwise None."""

    account_id: str
    chat: object = None
    message: object = None


_MENUS = ("main", "chat", "profile", "message")
_CHAT_KINDS = ("user", "chat", "channel")


def add_menu_item(menu, text, callback, kinds=None):
    """Adds an item to a Telegram menu, after its own items.

    menu is "main" (the side menu), "chat" (the chat's "⋮" menu and its
    context menu in the chat list), "profile" (the profile's menu), or
    "message" (a message's context menu).
    callback(context) gets a MenuContext; it may be a coroutine function.
    kinds limits a "chat", "profile" or "message" item to these kinds: "user",
    "chat" (basic group), "channel" (also supergroups). The item shows from
    the next time the menu opens. Returns a rule with remove()."""
    _choice(menu, _MENUS, "menu")
    if not isinstance(text, str) or not 1 <= len(text.strip()) <= 64:
        raise ValueError("Текст пункта меню: от 1 до 64 символов.")
    if not callable(callback):
        raise TypeError("callback должен быть функцией.")
    if kinds is not None:
        if menu == "main":
            raise ValueError("kinds применим только к меню chat, profile и message.")
        kinds = list(kinds)
        if not kinds or any(kind not in _CHAT_KINDS for kind in kinds):
            raise ValueError("kinds: непустой список из user, chat, channel.")

    def clicked(value):
        from .models import chat_from_native, message_from_native
        chat = value.get("chat")
        message = value.get("message")
        return callback(MenuContext(
            str(value.get("account_id", "")),
            chat_from_native(chat) if chat else None,
            message_from_native(message) if message else None))

    return WidgetRule(context(), "ui.menu.add", clicked, menu=menu, text=text.strip(),
                      kinds=kinds or [])


@dataclass(frozen=True)
class PanelContext:
    """The account and chat of the panel the user actually clicked."""

    account_id: str
    chat: object
    panel: str
    handle: str = ""


def _panel_parameters(panel, text, tooltip, width, priority, enabled, visible, chat, kinds):
    _choice(panel, ("chat.top_bar", "compose", "search.actions"), "panel")
    if not isinstance(text, str) or not text.strip() or len(text.encode("utf-16-le")) > 64:
        raise ValueError("Текст кнопки: непустая строка до 32 единиц UTF-16.")
    if not isinstance(tooltip, str) or len(tooltip.encode("utf-16-le")) > 512:
        raise ValueError("Подсказка кнопки: строка до 256 единиц UTF-16.")
    if type(width) is not int or not 32 <= width <= 160:
        raise ValueError("width: целое число от 32 до 160.")
    if type(priority) is not int or not -10000 <= priority <= 10000:
        raise ValueError("priority: целое число от -10000 до 10000.")
    if type(enabled) is not bool or type(visible) is not bool:
        raise ValueError("enabled и visible должны быть bool.")
    if kinds is not None:
        if isinstance(kinds, str):
            raise ValueError("kinds: список из user, chat, channel.")
        kinds = list(kinds)
        if not kinds or any(kind not in _CHAT_KINDS for kind in kinds):
            raise ValueError("kinds: непустой список из user, chat, channel.")
    account_id = chat_id = ""
    if chat is not None:
        account_id = str(getattr(chat, "account_id", ""))
        chat_id = str(getattr(chat, "id", ""))
        if not account_id.isascii() or not account_id.isdecimal() or int(account_id) <= 0 \
                or not chat_id.isascii() or not chat_id.isdecimal() or int(chat_id) <= 0:
            raise TypeError("chat должен быть Chat из desktop.client с аккаунтом и ID.")
    return {"panel": panel, "text": text.strip(), "tooltip": tooltip, "width": width,
            "priority": priority, "enabled": enabled, "visible": visible,
            "account_id": account_id, "chat_id": chat_id, "kinds": kinds or []}


class PanelButton(WidgetRule):
    """A native button in a documented Telegram panel, removed on unload.

    Higher priorities receive space first. Buttons that do not fit are
    hidden; they never cover Telegram's controls. Optional chat/kinds filters
    apply to every open window. callback receives a PanelContext.
    """

    def __init__(self, panel, text, callback, *, tooltip="", width=40, priority=0,
                 enabled=True, visible=True, chat=None, kinds=None):
        if not callable(callback):
            raise TypeError("callback должен быть функцией.")
        self.panel = panel
        self._chat = chat
        self._kinds = None if kinds is None else list(kinds) if not isinstance(kinds, str) else kinds
        self._parameters = _panel_parameters(panel, text, tooltip, width, priority,
                                              enabled, visible, chat, self._kinds)

        def clicked(value):
            from .models import chat_from_native
            parameters = self._parameters
            if self.removed or not parameters["visible"] or not parameters["enabled"] \
                    or value.get("panel") != self.panel:
                return
            account_id = str(value["account_id"])
            target = chat_from_native(value["chat"])
            if target.account_id != account_id:
                raise ValueError("Аккаунт панели не совпадает с аккаунтом чата.")
            if parameters["account_id"] and (parameters["account_id"] != account_id
                    or parameters["chat_id"] != str(target.id)):
                return
            if parameters["kinds"] and target.kind not in parameters["kinds"]:
                return
            return callback(PanelContext(account_id, target, self.panel, str(value.get("handle", ""))))

        super().__init__(context(), "ui.panel.add", clicked, **self._parameters)

    def update(self, text=None, tooltip=None, width=None, priority=None, enabled=None, visible=None):
        with self.owner.manager.runtime.lock:
            if self.removed or not self.owner.active:
                raise ObjectDestroyed("Кнопка удалена или плагин уже выгружен.")
            changes = {"text": text, "tooltip": tooltip, "width": width,
                       "priority": priority, "enabled": enabled, "visible": visible}
            values = {key: value for key, value in changes.items() if value is not None}
            candidate = {**self._parameters, **values}
            parameters = _panel_parameters(self.panel, candidate["text"], candidate["tooltip"],
                                           candidate["width"], candidate["priority"], candidate["enabled"],
                                           candidate["visible"], self._chat, self._kinds)
            self.owner.manager.runtime.submit_ui(self.owner, "ui.panel.add", rule_id=self.id, **parameters)
            self._parameters = parameters

    def show(self):
        self.update(visible=True)

    def hide(self):
        self.update(visible=False)


def add_panel_button(panel, text, callback, *, tooltip="", width=40, priority=0,
                     enabled=True, visible=True, chat=None, kinds=None):
    return PanelButton(panel, text, callback, tooltip=tooltip, width=width, priority=priority,
                       enabled=enabled, visible=visible, chat=chat, kinds=kinds)
