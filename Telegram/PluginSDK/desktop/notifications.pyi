from .models import Chat
from .ui import WidgetRule

def add_rule(*, chat: Chat | None = None, contains: str = "", replacement: str = "",
             hide: bool = False, group_seconds: int = 0, priority: int = 0) -> WidgetRule: ...
