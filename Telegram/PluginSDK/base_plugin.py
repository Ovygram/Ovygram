"""Plugin lifecycle and persistent settings for Ovygram."""

# Also importable from here, as in Android plugins.
from hook_utils import HookResult, HookStrategy  # noqa: F401


class BasePlugin:
    def on_plugin_load(self):
        pass

    def on_plugin_unload(self):
        pass

    def create_settings(self):
        return []

    def get_setting(self, key, default=None):
        return self._desktop_context.get(key, default)

    def set_setting(self, key, value):
        self._desktop_context.set(key, value)

    def get_account_setting(self, account_id, key, default=None):
        return self._desktop_context.get(key, default, account_id)

    def set_account_setting(self, account_id, key, value):
        self._desktop_context.set(key, value, account_id)

    def log(self, message):
        self._desktop_context.log(str(message))

    def subscribe(self, event, callback, priority=0):
        return self._desktop_context.manager.runtime.register(
            self._desktop_context, event, callback, priority)

    def add_hook(self, point, callback, priority=0, request=None):
        return self._desktop_context.manager.runtime.register(
            self._desktop_context, point, callback, priority, request)

    def run_async(self, coroutine):
        return self._desktop_context.manager.runtime.schedule(self._desktop_context, coroutine)

    def get_language(self):
        """Return Telegram's active language code (e.g. 'en', 'ru', 'de')."""
        from ovygram_i18n import get_language
        return get_language()

    def get_base_language(self):
        """Return the base language pack code for derived packs, or ''."""
        from ovygram_i18n import get_base_language
        return get_base_language()

    def tr(self, key_or_map, default=None, *, language=None, **kwargs):
        """Translate a `{lang: text}` mapping or a string key using `self.TRANSLATIONS` and `locales/<lang>.json`."""
        from desktop import i18n
        if isinstance(key_or_map, dict):
            return i18n.pick(key_or_map, default, language=language, **kwargs)
        translator = getattr(self, "_desktop_translator", None)
        if translator is None:
            inline = getattr(self, "TRANSLATIONS", None)
            directory = getattr(self._desktop_context, "directory", None)
            translator = i18n.Translator(inline if isinstance(inline, dict) else None, directory=directory)
            self._desktop_translator = translator
        return translator.tr(key_or_map, default=default, language=language, **kwargs)
