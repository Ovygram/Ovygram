from . import tl
from ._runtime import OutgoingMessage, RpcContext, RpcError, RpcResponse, UpdateContext
from .models import Account, Attachment, Chat, Event, Message, Progress, User


def supports(operation: str) -> bool:
    """Whether this SDK publishes the named module function or type."""
    from importlib import import_module
    if not isinstance(operation, str):
        return False
    module, separator, name = operation.partition(".")
    if not separator or not name.isidentifier() or name.startswith("_"):
        return False
    if module not in ("actions", "client", "commands", "components", "i18n", "media",
                      "lists", "native", "notifications", "search", "ui"):
        return False
    api = import_module(f".{module}", __name__)
    value = getattr(api, name, None)
    return callable(value) and getattr(value, "__module__", None) == api.__name__
