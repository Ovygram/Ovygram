"""Shared localization helpers for Ovygram's Python runtime and Plugin SDK.

Standard library only. Mirrors Telegram/SourceFiles/ovygram/i18n.cpp so the
client UI, trusted manager and plugin host resolve languages identically.
"""

import json
from pathlib import Path
import re
import threading


_LANG_CODE = re.compile(r"[a-z0-9-]{1,32}\Z")
_LOCK = threading.Lock()
_LANGUAGE = "en"
_BASE_LANGUAGE = ""


def normalize_language(code) -> str:
    if not isinstance(code, str):
        return ""
    text = code.strip().lower().replace("_", "-")
    if not text or text.startswith("#"):
        return ""
    text = text.strip("-")
    if not text or not _LANG_CODE.fullmatch(text):
        return ""
    return text


def primary_language(code) -> str:
    normalized = normalize_language(code)
    head, _, _ = normalized.partition("-")
    return head


def set_language(language, base_language="") -> tuple[str, str]:
    global _LANGUAGE, _BASE_LANGUAGE
    norm_lang = normalize_language(language)
    norm_base = normalize_language(base_language)
    if not norm_lang:
        norm_lang = norm_base or "en"
    if norm_base == norm_lang:
        norm_base = ""
    with _LOCK:
        _LANGUAGE = norm_lang
        _BASE_LANGUAGE = norm_base
        return _LANGUAGE, _BASE_LANGUAGE


def get_language() -> str:
    with _LOCK:
        return _LANGUAGE


def get_base_language() -> str:
    with _LOCK:
        return _BASE_LANGUAGE


def is_russian() -> bool:
    with _LOCK:
        return primary_language(_LANGUAGE) == "ru" or primary_language(_BASE_LANGUAGE) == "ru"


def ui_language() -> str:
    return "ru" if is_russian() else "en"


def tr(en: str, ru: str) -> str:
    return ru if is_russian() else en


def _candidate_codes(language=None, base_language=None) -> list[str]:
    if language is None:
        with _LOCK:
            active = _LANGUAGE
            base = _BASE_LANGUAGE if base_language is None else normalize_language(base_language)
    else:
        active = normalize_language(language) or get_language()
        base = normalize_language(base_language) if base_language is not None else ""
    order = []
    for code in (active, primary_language(active), base, primary_language(base), "en"):
        if code and code not in order:
            order.append(code)
    return order


def format_text(text: str, kwargs: dict) -> str:
    if not kwargs:
        return text
    try:
        return text.format(**kwargs)
    except (KeyError, IndexError, ValueError):
        return text


def pick_translation(translations, default=None, *, language=None, base_language=None, **kwargs) -> str:
    """Choose the best translation for the active Telegram language.

    Fallback order:
      1. Exact active language (e.g. "pt-br", "ru", "de")
      2. Primary subtag of active language (e.g. "pt")
      3. Base language pack and its primary subtag (for derived packs)
      4. English ("en" or any "en-*") if provided
      5. Explicit `default` if provided and non-empty
      6. Whatever non-empty translation is available in `translations`
    """
    normalized = {}
    if isinstance(translations, dict):
        for raw_key, value in translations.items():
            key = normalize_language(raw_key)
            if key and isinstance(value, str) and value.strip():
                normalized.setdefault(key, value)

    for code in _candidate_codes(language, base_language):
        if code in normalized:
            return format_text(normalized[code], kwargs)

    for key, value in normalized.items():
        if primary_language(key) == "en":
            return format_text(value, kwargs)

    if isinstance(default, str) and default.strip():
        return format_text(default, kwargs)

    if normalized:
        return format_text(next(iter(normalized.values())), kwargs)

    return format_text(default, kwargs) if isinstance(default, str) else ""


def load_locale_directory(directory: Path, subdir: str = "locales") -> dict[str, dict[str, str]]:
    """Read `<lang>.json` translation tables from a plugin's package directory."""
    if not isinstance(subdir, str) or not subdir or "/" in subdir or "\\" in subdir or subdir in (".", ".."):
        raise ValueError(tr(
            "Locale directory must be a single folder name inside the plugin.",
            "Каталог переводов должен быть именем одной папки внутри плагина.",
        ))
    root = Path(directory).resolve()
    target = (root / subdir).resolve()
    if not target.is_relative_to(root) or not target.is_dir():
        return {}
    catalog = {}
    for entry in sorted(target.iterdir(), key=lambda item: item.name.lower()):
        if entry.suffix.lower() != ".json" or not entry.is_file() or entry.is_symlink():
            continue
        code = normalize_language(entry.stem)
        if not code:
            continue
        resolved = entry.resolve()
        if not resolved.is_relative_to(target):
            continue
        try:
            data = json.loads(resolved.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, ValueError):
            continue
        if isinstance(data, dict):
            table = {
                key: value for key, value in data.items()
                if isinstance(key, str) and isinstance(value, str)
            }
            if table:
                catalog[code] = table
    return catalog
