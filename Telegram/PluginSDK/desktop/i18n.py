"""Localization helpers for Ovygram plugins.

Plugins follow Telegram's active language setting automatically:
  active language -> primary tag -> base language -> "en" (if provided) ->
  explicit default -> whatever translation the plugin provides.
"""

from pathlib import Path

from ._runtime import context
from ovygram_i18n import (
    format_text,
    get_base_language,
    get_language,
    load_locale_directory,
    normalize_language,
    pick_translation,
    primary_language,
    tr as _sys_tr,
    ui_language as _ui_language,
)


def language() -> str:
    """Return the active Telegram language code (e.g. 'en', 'ru', 'de', 'pt-br')."""
    return get_language()


def base_language() -> str:
    """Return the base language pack code if the user uses a derived pack, else ''."""
    return get_base_language()


def ui_language() -> str:
    """Return Ovygram's resolved system UI language ('ru' or 'en')."""
    return _ui_language()


def pick(translations, default=None, *, language=None, **kwargs) -> str:
    """Pick a localized string from a `{lang: text}` mapping.

    Fallback order: Telegram's language -> its primary tag -> base language ->
    'en' (if present) -> default -> first available entry in `translations`.
    """
    if not isinstance(translations, dict):
        raise TypeError(_sys_tr(
            "translations must be a dict of {language: text}.",
            "translations должен быть словарём {язык: текст}.",
        ))
    return pick_translation(translations, default, language=language, **kwargs)


class Translator:
    """Key-based translator backed by `{lang: {key: text}}` tables or `locales/<lang>.json`."""

    def __init__(self, translations=None, *, directory=None, subdir="locales"):
        self._catalog: dict[str, dict[str, str]] = {}
        if directory is not None:
            for code, table in load_locale_directory(Path(directory), subdir=subdir).items():
                self._catalog[code] = dict(table)
        if translations is not None:
            self.add(translations)

    def add(self, translations):
        """Merge `{lang: {key: text}}` entries into this translator."""
        if not isinstance(translations, dict):
            raise TypeError(_sys_tr(
                "translations must be a dict of {language: {key: text}}.",
                "translations должен быть словарём {язык: {ключ: текст}}.",
            ))
        for raw_lang, table in translations.items():
            code = normalize_language(raw_lang)
            if not code or not isinstance(table, dict):
                continue
            bucket = self._catalog.setdefault(code, {})
            for key, value in table.items():
                if isinstance(key, str) and isinstance(value, str):
                    bucket[key] = value

    def available_languages(self) -> tuple[str, ...]:
        return tuple(self._catalog.keys())

    def tr(self, key: str, default=None, *, language=None, **kwargs) -> str:
        if not isinstance(key, str):
            raise TypeError(_sys_tr(
                "Translation key must be a string.",
                "Ключ перевода должен быть строкой.",
            ))
        candidates = {}
        for code, table in self._catalog.items():
            if key in table:
                candidates[code] = table[key]
        if candidates:
            return pick_translation(candidates, default=default, language=language, **kwargs)
        fallback = default if isinstance(default, str) else key
        return format_text(fallback, kwargs)

    __call__ = tr


def load_locales(subdir: str = "locales", translations=None) -> Translator:
    """Load `<lang>.json` files from the current plugin's package directory."""
    return Translator(translations, directory=context().directory, subdir=subdir)


def tr(key_or_map, default=None, *, language=None, **kwargs) -> str:
    """Translate either a `{lang: text}` dict or a key from the active plugin's locales."""
    if isinstance(key_or_map, dict):
        return pick(key_or_map, default, language=language, **kwargs)
    plugin_context = context()
    translator = getattr(plugin_context, "_translator", None)
    if translator is None:
        translator = Translator(directory=plugin_context.directory)
        plugin_context._translator = translator
    return translator.tr(key_or_map, default=default, language=language, **kwargs)
