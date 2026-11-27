"""Tests for Ovygram C++ and Python localization (i18n) and Plugin SDK desktop.i18n."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / "Telegram" / "SourceFiles"
SDK = ROOT / "Telegram" / "PluginSDK"
sys.path.insert(0, str(SDK))

import desktop
import desktop.i18n as sdk_i18n
import ovygram_i18n
from ovygram_package import InvalidPlugin, read_package
from ovygram_runtime import InProcessManager, Manager

HEADER = '''from base_plugin import BasePlugin
import desktop
from desktop import i18n

__id__ = "i18n_demo"
__name__ = "Demo Plugin"
__name_i18n__ = {"ru": "Демо-плагин", "de": "Demo-Erweiterung"}
__version__ = "1.0.0"
__author__ = "Tests"
__description__ = "English description."
__description_i18n__ = {"ru": "Русское описание.", "de": "Deutsche Beschreibung."}
__platform__ = "desktop"
__desktop_sdk__ = 1
__requirements__ = []
'''


def _find_qt6():
    candidates = [
        Path(os.environ.get("QT6_PREFIX", "")) if os.environ.get("QT6_PREFIX") else None,
        Path("/tmp/ovygram-qt/6.11.0/gcc_64"),
        Path("/usr"),
    ]
    for prefix in candidates:
        if not prefix:
            continue
        inc = prefix / "include"
        if not (inc / "QtCore" / "QString").exists():
            alt = prefix / "include" / "x86_64-linux-gnu" / "qt6"
            if (alt / "QtCore" / "QString").exists():
                inc = alt
            else:
                continue
        for lib_candidate in (prefix / "lib", prefix / "lib" / "x86_64-linux-gnu"):
            if (lib_candidate / "libQt6Core.so").exists() or (lib_candidate / "libQt6Core.so.6").exists():
                return inc, lib_candidate
    return None, None


class CppI18nTests(unittest.TestCase):
    def test_cpp_i18n_language_resolution_and_channel_labels(self):
        if shutil.which("g++") is None:
            self.skipTest("g++ is not installed")
        qt_include, qt_lib = _find_qt6()
        if not qt_include or not qt_lib:
            self.skipTest("Qt6Core headers/libraries not found")

        driver = r'''
#include "ovygram/i18n.h"
#include "ovygram/auto_update_verify.h"

#include <QJsonObject>
#include <iostream>
#include <stdexcept>
#include <string>

static void Require(bool cond, const std::string &msg) {
    if (!cond) {
        throw std::runtime_error(msg);
    }
}

int main() {
    // Default language is English.
    Require(Ovygram::I18n::Language() == QStringLiteral("en"), "default lang must be en");
    Require(!Ovygram::I18n::IsRussian(), "default must not be Russian");
    Require(Ovygram::I18n::UiLanguage() == QStringLiteral("en"), "default UI lang must be en");
    Require(Ovygram::Tr(u"Plugins", u"Плагины") == QStringLiteral("Plugins"), "default Tr must be EN");
    Require(Ovygram::UpdateChannelTitle(Ovygram::UpdateChannel::Stable) == QStringLiteral("Stable"),
            "Stable title in EN");

    int notifications = 0;
    const auto listenerId = Ovygram::I18n::RegisterListener([&] { ++notifications; });

    // Switch to Russian (with region subtag).
    Ovygram::I18n::SetLanguage(QStringLiteral("ru_RU"), QString());
    Require(notifications == 1, "listener called once");
    Require(Ovygram::I18n::Language() == QStringLiteral("ru-ru"), "normalized ru-ru");
    Require(Ovygram::I18n::IsRussian(), "ru-ru must be Russian");
    Require(Ovygram::I18n::UiLanguage() == QStringLiteral("ru"), "UI lang must be ru");
    Require(Ovygram::Tr(u"Plugins", u"Плагины") == QStringLiteral("Плагины"), "Tr must be RU");
    Require(Ovygram::UpdateChannelTitle(Ovygram::UpdateChannel::Stable) == QStringLiteral("Стабильный"),
            "Stable title in RU");

    // Plural forms in Russian.
    Require(Ovygram::TrCount(1, u"%1 plugin", u"%1 plugins", u"%1 плагин", u"%1 плагина", u"%1 плагинов").arg(1)
            == QStringLiteral("1 плагин"), "RU 1");
    Require(Ovygram::TrCount(2, u"%1 plugin", u"%1 plugins", u"%1 плагин", u"%1 плагина", u"%1 плагинов").arg(2)
            == QStringLiteral("2 плагина"), "RU 2");
    Require(Ovygram::TrCount(5, u"%1 plugin", u"%1 plugins", u"%1 плагин", u"%1 плагина", u"%1 плагинов").arg(5)
            == QStringLiteral("5 плагинов"), "RU 5");
    Require(Ovygram::TrCount(11, u"%1 plugin", u"%1 plugins", u"%1 плагин", u"%1 плагина", u"%1 плагинов").arg(11)
            == QStringLiteral("11 плагинов"), "RU 11");
    Require(Ovygram::TrCount(21, u"%1 plugin", u"%1 plugins", u"%1 плагин", u"%1 плагина", u"%1 плагинов").arg(21)
            == QStringLiteral("21 плагин"), "RU 21");

    // Derived custom pack based on Russian stays Russian.
    Ovygram::I18n::SetLanguage(QStringLiteral("#custom"), QStringLiteral("ru"));
    Require(Ovygram::I18n::IsRussian(), "custom based on ru is Russian");

    // Unsupported language falls back to English UI while keeping active code for plugins.
    Ovygram::I18n::SetLanguage(QStringLiteral("pt-BR"), QString());
    Require(Ovygram::I18n::Language() == QStringLiteral("pt-br"), "preserves pt-br");
    Require(!Ovygram::I18n::IsRussian(), "pt-br is not Russian");
    Require(Ovygram::I18n::UiLanguage() == QStringLiteral("en"), "pt-br UI falls back to en");
    Require(Ovygram::TrCount(1, u"%1 plugin", u"%1 plugins", u"%1 плагин", u"%1 плагина", u"%1 плагинов").arg(1)
            == QStringLiteral("1 plugin"), "EN 1");
    Require(Ovygram::TrCount(2, u"%1 plugin", u"%1 plugins", u"%1 плагин", u"%1 плагина", u"%1 плагинов").arg(2)
            == QStringLiteral("2 plugins"), "EN 2");

    // Plugin metadata fallback chain: exact -> primary -> base -> en -> fallback -> first available.
    QJsonObject mapPt;
    mapPt.insert(QStringLiteral("pt"), QStringLiteral("Portugues"));
    mapPt.insert(QStringLiteral("en"), QStringLiteral("English"));
    Require(Ovygram::I18n::PickLocalized(QStringLiteral("Fallback"), mapPt) == QStringLiteral("Portugues"),
            "pt-br should match primary pt");

    QJsonObject mapEnOnly;
    mapEnOnly.insert(QStringLiteral("ru"), QStringLiteral("Русский"));
    mapEnOnly.insert(QStringLiteral("en"), QStringLiteral("English"));
    Require(Ovygram::I18n::PickLocalized(QStringLiteral("Fallback"), mapEnOnly) == QStringLiteral("English"),
            "pt-br should fall back to en");

    QJsonObject mapDeOnly;
    mapDeOnly.insert(QStringLiteral("de"), QStringLiteral("Deutsch"));
    Require(Ovygram::I18n::PickLocalized(QString(), mapDeOnly) == QStringLiteral("Deutsch"),
            "no en or fallback -> first available");

    QJsonObject pluginObj;
    pluginObj.insert(QStringLiteral("name"), QStringLiteral("Default Name"));
    pluginObj.insert(QStringLiteral("name_i18n"), mapPt);
    Require(Ovygram::I18n::PluginName(pluginObj) == QStringLiteral("Portugues"), "PluginName localized");

    Ovygram::I18n::UnregisterListener(listenerId);
    Ovygram::I18n::SetLanguage(QStringLiteral("en"), QString());
    Require(notifications == 3, "no notification after unregister");

    std::cout << "CPP_I18N_OK\n";
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            tmp_dir = Path(tmp)
            driver_path = tmp_dir / "driver.cpp"
            driver_path.write_text(driver, encoding="utf-8")
            binary = tmp_dir / "i18n_test"
            subprocess.check_call([
                "g++", "-std=c++20", "-fPIC", "-Wall", "-Wextra", "-Werror",
                f"-I{SRC}", f"-I{qt_include}", f"-I{qt_include / 'QtCore'}",
                str(SRC / "ovygram" / "i18n.cpp"),
                str(SRC / "ovygram" / "auto_update_verify.cpp"),
                str(driver_path),
                f"-L{qt_lib}", f"-Wl,-rpath,{qt_lib}",
                "-lQt6Core", "-lcrypto",
                "-o", str(binary),
            ])
            out = subprocess.check_output([str(binary)], text=True)
            self.assertIn("CPP_I18N_OK", out)


class PythonI18nCoreTests(unittest.TestCase):
    def setUp(self):
        self.addCleanup(ovygram_i18n.set_language, "en", "")
        ovygram_i18n.set_language("en", "")

    def test_language_resolution_and_system_tr(self):
        self.assertEqual(sdk_i18n.language(), "en")
        self.assertEqual(sdk_i18n.ui_language(), "en")
        self.assertEqual(ovygram_i18n.tr("Open", "Открыть"), "Open")

        ovygram_i18n.set_language("ru_RU")
        self.assertEqual(sdk_i18n.language(), "ru-ru")
        self.assertEqual(sdk_i18n.ui_language(), "ru")
        self.assertEqual(ovygram_i18n.tr("Open", "Открыть"), "Открыть")

        ovygram_i18n.set_language("#custom", "ru")
        self.assertEqual(sdk_i18n.language(), "ru")
        self.assertEqual(sdk_i18n.ui_language(), "ru")

        ovygram_i18n.set_language("uk", "")
        self.assertEqual(sdk_i18n.language(), "uk")
        self.assertEqual(sdk_i18n.ui_language(), "en")
        self.assertEqual(ovygram_i18n.tr("Open", "Открыть"), "Open")

    def test_pick_fallback_order_matches_spec(self):
        ovygram_i18n.set_language("pt-br", "es")
        # 1. Exact active language.
        self.assertEqual(sdk_i18n.pick({"pt-br": "Brasil", "pt": "Portugal", "en": "English"}), "Brasil")
        # 2. Primary tag of active language.
        self.assertEqual(sdk_i18n.pick({"pt": "Portugal", "es": "Espanol", "en": "English"}), "Portugal")
        # 3. Base language pack.
        self.assertEqual(sdk_i18n.pick({"es": "Espanol", "en": "English"}), "Espanol")
        # 4. English fallback when active/base missing.
        self.assertEqual(sdk_i18n.pick({"de": "Deutsch", "en": "English"}), "English")
        self.assertEqual(sdk_i18n.pick({"de": "Deutsch", "en-us": "US English"}), "US English")
        # 5. Explicit default when no English exists.
        self.assertEqual(sdk_i18n.pick({"de": "Deutsch"}, default="Fallback"), "Fallback")
        # 6. Whatever exists in the plugin when neither active, en, nor default exists.
        self.assertEqual(sdk_i18n.pick({"de": "Deutsch", "ja": "日本語"}), "Deutsch")
        # Formatting & safe fallback on unknown placeholder.
        self.assertEqual(sdk_i18n.pick({"en": "Hello, {name}!"}, name="Ovy"), "Hello, Ovy!")
        self.assertEqual(sdk_i18n.pick({"en": "Hello, {missing}!"}, name="Ovy"), "Hello, {missing}!")

    def test_desktop_supports_exposes_i18n_api(self):
        self.assertTrue(desktop.supports("i18n.language"))
        self.assertTrue(desktop.supports("i18n.base_language"))
        self.assertTrue(desktop.supports("i18n.ui_language"))
        self.assertTrue(desktop.supports("i18n.pick"))
        self.assertTrue(desktop.supports("i18n.Translator"))
        self.assertTrue(desktop.supports("i18n.load_locales"))
        self.assertTrue(desktop.supports("i18n.tr"))

    def test_package_metadata_i18n_and_v010_compatibility(self):
        source = (HEADER + "\nclass Demo(BasePlugin):\n    pass\n").encode("utf-8")
        meta = read_package(source, "i18n_demo.ovyplugin")
        self.assertEqual(meta["name_i18n"], {"ru": "Демо-плагин", "de": "Demo-Erweiterung"})
        self.assertEqual(meta["description_i18n"], {"ru": "Русское описание.", "de": "Deutsche Beschreibung."})

        # Without i18n tags, dicts are empty (omitted from stored state.json for v0.1.0 compatibility).
        plain = '''from base_plugin import BasePlugin
__id__ = "plain"
__name__ = "Plain"
__version__ = "1.0.0"
__author__ = "Tests"
__description__ = "Plain."
__platform__ = "desktop"
__desktop_sdk__ = 1
__requirements__ = []
class Plain(BasePlugin):
    pass
'''.encode("utf-8")
        plain_meta = read_package(plain, "plain.ovyplugin")
        self.assertEqual(plain_meta["name_i18n"], {})
        self.assertEqual(plain_meta["description_i18n"], {})

        # Invalid language code is rejected in active language (EN and RU).
        bad = plain + b'\n__name_i18n__ = {"bad code!": "Oops"}\n'
        ovygram_i18n.set_language("en")
        with self.assertRaisesRegex(InvalidPlugin, "valid language code"):
            read_package(bad, "plain.ovyplugin")
        ovygram_i18n.set_language("ru")
        with self.assertRaisesRegex(InvalidPlugin, "корректный код языка"):
            read_package(bad, "plain.ovyplugin")


class PluginRuntimeI18nTests(unittest.TestCase):
    kind = InProcessManager

    def setUp(self):
        self.addCleanup(ovygram_i18n.set_language, "en", "")
        ovygram_i18n.set_language("en", "")
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.manager = self.kind(self.root / "profile")
        self.addCleanup(self.manager.shutdown)

    def dispatch(self, action, **params):
        return json.loads(self.manager.dispatch(json.dumps({"action": action, **params})))

    def build_locale_zip(self) -> Path:
        archive = self.root / "i18n_demo.ovyzip"
        main_py = HEADER + '''
class Demo(BasePlugin):
    TRANSLATIONS = {
        "en": {"inline": "Hello {who}"},
        "ru": {"inline": "Привет, {who}"},
    }

    def on_plugin_load(self):
        self.record_snapshot("load")
        self._unsub = self.subscribe("language_changed", self.on_language)

    def on_plugin_unload(self):
        if getattr(self, "_unsub", None):
            self._unsub.cancel()

    def on_language(self, event):
        self.record_snapshot("event", event.details)

    def record_snapshot(self, source, event=None):
        history = list(self.get_setting("history", []))
        history.append({
            "source": source,
            "lang": self.get_language(),
            "base": self.get_base_language(),
            "event": event,
            "inline": self.tr("inline", who="Ovy"),
            "locale": self.tr("greeting"),
            "dict": self.tr({"ru": "Кнопка", "de": "Taste", "en": "Button"}),
            "only_de": self.tr({"de": "Nur Deutsch"}),
        })
        self.set_setting("history", history)
'''
        with ZipFile(archive, "w") as zf:
            zf.writestr("plugin.json", json.dumps({"id": "i18n_demo", "entry": "main.py"}))
            zf.writestr("main.py", main_py)
            zf.writestr("locales/en.json", json.dumps({"greeting": "Welcome"}))
            zf.writestr("locales/ru.json", json.dumps({"greeting": "Добро пожаловать"}))
            zf.writestr("locales/de.json", json.dumps({"greeting": "Willkommen"}))
        return archive

    def test_initial_and_live_language_switching_with_locales_and_events(self):
        # Set language to Russian before enabling plugin so on_plugin_load sees "ru".
        res = self.dispatch("set_language", language="ru", base_language="")
        self.assertNotIn("error", res)
        self.assertEqual(res["result"], {"language": "ru", "base_language": "", "ui_language": "ru"})

        archive = self.build_locale_zip()
        identity = self.manager.install(str(archive))
        # Snapshot & metadata preserve name_i18n and description_i18n.
        info = self.manager.describe(identity)
        self.assertEqual(info["name_i18n"]["ru"], "Демо-плагин")
        self.assertEqual(info["description_i18n"]["de"], "Deutsche Beschreibung.")

        self.manager.enable(identity)
        history = self.manager.records[identity]["settings"]["history"]
        self.assertEqual(len(history), 1)
        self.assertEqual(history[0]["source"], "load")
        self.assertEqual(history[0]["lang"], "ru")
        self.assertEqual(history[0]["inline"], "Привет, Ovy")
        self.assertEqual(history[0]["locale"], "Добро пожаловать")
        self.assertEqual(history[0]["dict"], "Кнопка")
        self.assertEqual(history[0]["only_de"], "Nur Deutsch")

        # Live switch to German (supported by locales/de.json and inline dict, falls back to EN for TRANSLATIONS).
        self.dispatch("set_language", language="de", base_language="")
        self.dispatch("client_event", event={
            "point": "language_changed",
            "account_id": "",
            "details": {"language": "de", "base_language": "", "ui_language": "en"},
        })
        history = self.manager.records[identity]["settings"]["history"]
        self.assertEqual(len(history), 2)
        self.assertEqual(history[1]["source"], "event")
        self.assertEqual(history[1]["lang"], "de")
        self.assertEqual(history[1]["event"], {"language": "de", "base_language": "", "ui_language": "en"})
        self.assertEqual(history[1]["inline"], "Hello Ovy")
        self.assertEqual(history[1]["locale"], "Willkommen")
        self.assertEqual(history[1]["dict"], "Taste")
        self.assertEqual(history[1]["only_de"], "Nur Deutsch")

        # Live switch to Ukrainian (unsupported by plugin -> falls back to EN, or first available for only_de).
        self.dispatch("set_language", language="uk", base_language="")
        self.dispatch("client_event", event={
            "point": "language_changed",
            "account_id": "",
            "details": {"language": "uk", "base_language": "", "ui_language": "en"},
        })
        history = self.manager.records[identity]["settings"]["history"]
        self.assertEqual(len(history), 3)
        self.assertEqual(history[2]["lang"], "uk")
        self.assertEqual(history[2]["inline"], "Hello Ovy")
        self.assertEqual(history[2]["locale"], "Welcome")
        self.assertEqual(history[2]["dict"], "Button")
        self.assertEqual(history[2]["only_de"], "Nur Deutsch")

    def test_v010_plugin_without_i18n_omits_i18n_keys_in_stored_metadata(self):
        plugin = self.root / "plain.ovyplugin"
        plugin.write_text('''from base_plugin import BasePlugin
__id__ = "plain_v010"
__name__ = "Plain"
__version__ = "1.0.0"
__author__ = "Tests"
__description__ = "Plain."
__platform__ = "desktop"
__desktop_sdk__ = 1
__requirements__ = []
class Plain(BasePlugin):
    pass
''', encoding="utf-8")
        identity = self.manager.install(str(plugin))
        stored_meta = self.manager.records[identity]["current"]["metadata"]
        self.assertNotIn("name_i18n", stored_meta)
        self.assertNotIn("description_i18n", stored_meta)


@unittest.skipUnless(shutil.which("bwrap") is not None, "needs bubblewrap")
class ProcessHostI18nTests(PluginRuntimeI18nTests):
    kind = Manager


if __name__ == "__main__":
    unittest.main()
