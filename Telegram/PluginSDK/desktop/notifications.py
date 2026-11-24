"""Native desktop banner rules. The client applies these after preview privacy checks."""
from ._runtime import context
from .models import Chat
from .ui import WidgetRule


def add_rule(*, chat=None, contains="", replacement="", hide=False, group_seconds=0, priority=0):
    """Match a visible preview, replace its body, hide it or group repeated banners."""
    if chat is not None and not isinstance(chat, Chat):
        raise TypeError("chat должен быть Chat.")
    if type(hide) is not bool:
        raise TypeError("hide должен быть bool.")
    if not isinstance(contains, str) or len(contains.encode("utf-16-le")) > 512:
        raise ValueError("contains: не более 256 единиц UTF-16.")
    if not isinstance(replacement, str) or len(replacement.encode("utf-16-le")) > 1024:
        raise ValueError("replacement: не более 512 единиц UTF-16.")
    if type(group_seconds) is not int or not 0 <= group_seconds <= 3600:
        raise ValueError("group_seconds: от 0 до 3600.")
    if type(priority) is not int or not -10000 <= priority <= 10000:
        raise ValueError("priority: от -10000 до 10000.")
    if not (replacement or hide or group_seconds):
        raise ValueError("Укажите replacement, hide или group_seconds.")
    return WidgetRule(context(), "ui.notification.add", contains=contains, replacement=replacement,
                      hide=hide, group_seconds=group_seconds, priority=priority,
                      account_id=chat.account_id if chat else "", chat_id=str(chat.id) if chat else "")
