from collections.abc import Mapping
from os import PathLike
from typing import Any

def language() -> str: ...
def base_language() -> str: ...
def ui_language() -> str: ...
def pick(
    translations: Mapping[str, str],
    default: str | None = None,
    *,
    language: str | None = None,
    **kwargs: Any,
) -> str: ...

class Translator:
    def __init__(
        self,
        translations: Mapping[str, Mapping[str, str]] | None = None,
        *,
        directory: str | PathLike[str] | None = None,
        subdir: str = "locales",
    ) -> None: ...
    def add(self, translations: Mapping[str, Mapping[str, str]]) -> None: ...
    def available_languages(self) -> tuple[str, ...]: ...
    def tr(
        self,
        key: str,
        default: str | None = None,
        *,
        language: str | None = None,
        **kwargs: Any,
    ) -> str: ...
    def __call__(
        self,
        key: str,
        default: str | None = None,
        *,
        language: str | None = None,
        **kwargs: Any,
    ) -> str: ...

def load_locales(
    subdir: str = "locales",
    translations: Mapping[str, Mapping[str, str]] | None = None,
) -> Translator: ...

def tr(
    key_or_map: str | Mapping[str, str],
    default: str | None = None,
    *,
    language: str | None = None,
    **kwargs: Any,
) -> str: ...
