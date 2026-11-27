"""Plugin commands of the message field: the client's engine and the path
from desktop.commands to a handler."""

import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "Telegram/PluginSDK"))
from ovygram_commands import Engine, checked, split
from ovygram_runtime import InProcessManager, Manager

WEATHER = {"name": "погода", "aliases": ["weather"], "description": "Погода в городе", "args": [
    {"name": "город", "type": "str", "suggest": True},
    {"name": "дни", "type": "int", "optional": True, "default": 1, "choices": [1, 3, 7]}]}
SAY = {"name": "say", "kinds": ["user"], "args": [
    {"name": "loud", "type": "bool"}, {"name": "text", "type": "str", "rest": True}]}
CHAT = {"kind": "user", "account_id": "1", "chat_id": "5"}

PLUGIN = '''from base_plugin import BasePlugin
from desktop import commands

__id__ = "commands"
__name__ = "Commands"
__version__ = "1.0.0"
__author__ = "Tests"
__description__ = "Test plugin."
__platform__ = "desktop"
__desktop_sdk__ = 1
__requirements__ = []

CITIES = ["Москва", "Мурманск", "Нью-Йорк"]


class Commands(BasePlugin):
    def on_plugin_load(self):
        self.added = commands.add("погода", self.weather, description="Погода", args=[
            commands.Arg("город", suggest=self.cities),
            commands.Arg("дни", type=int, default=1, choices=[1, 3, 7])])
        commands.add("fail", self.fail)
        commands.add("later", self.later, args=[commands.Arg("city", suggest=self.cities_later)])

    def cities(self, prefix, context):
        self.set_setting("suggest", [prefix, context.kind, list(context.args)])
        return [(city, "город") for city in CITIES if city.lower().startswith(prefix.lower())]

    async def cities_later(self, prefix, context):
        return CITIES

    def weather(self, context, city, days):
        self.set_setting("ran", [context.account_id, city, days])
        if city == "remove":
            self.added.remove()

    async def later(self, context, city):
        pass

    def fail(self, context):
        raise RuntimeError("broken handler")
'''


class EngineTests(unittest.TestCase):
    def setUp(self):
        self.engine = Engine()
        self.engine.set("a" * 32, "weather", [WEATHER, SAY])

    def view(self, text, context=CHAT):
        return self.engine.input(text, len(text), context)

    def test_declarations_are_checked(self):
        for broken in (
            [{"name": "a b"}], [{"name": "x", "aliases": ["x"]}], [{"name": "x", "kinds": ["bot"]}],
            [{"name": "x", "args": [{"name": "n", "type": "date"}]}],
            [{"name": "x", "args": [{"name": "n", "type": "int", "choices": ["1"]}]}],
            [{"name": "x", "args": [{"name": "a", "type": "str", "optional": True},
                                    {"name": "b", "type": "str"}]}],
            [{"name": "x", "args": [{"name": "a", "type": "str", "rest": True},
                                    {"name": "b", "type": "str"}]}],
            [{"name": "x", "description": "a\nb"}], "x", [{"name": "x"}] * 65,
        ):
            with self.subTest(broken=broken), self.assertRaises(ValueError):
                checked(broken)

    def test_tokens(self):
        self.assertEqual(split('one "two three" fo'), (["one", "two three"], "fo", 16))
        self.assertEqual(split('one "two th'), (["one"], "two th", 4))
        self.assertEqual(split("one "), (["one"], "", 4))
        self.assertEqual(split("true keep  this ", 1), (["true"], "keep  this ", 5))

    def test_names_are_listed_filtered_and_completed(self):
        self.assertFalse(self.view("hello")["active"])
        self.assertFalse(self.view("//погода")["active"])
        self.assertFalse(self.view("/unknown")["active"])
        view = self.view("/")
        self.assertEqual([item["title"] for item in view["items"]], ["/say", "/weather", "/погода"])
        self.assertEqual(view["items"][2]["hint"], "<город> [дни]")
        self.assertEqual(view["items"][2]["plugin"], "weather")
        view = self.view("/по")
        self.assertEqual(([item["title"] for item in view["items"]], view["ghost"]), (["/погода"], "года"))
        self.assertEqual(self.engine.complete(), ("/погода ", 8))
        # Not available in a group: the command is neither listed nor run.
        self.assertFalse(self.view("/sa", {**CHAT, "kind": "chat"})["active"])
        self.assertIsNone(self.engine.submit("/say true hi", "chat"))

    def test_selection_and_escape(self):
        self.view("/")
        self.assertEqual(self.engine.move(1)["selected"], 1)
        self.assertEqual(self.engine.move(-1)["selected"], 0)
        self.assertEqual(self.engine.move(-1)["selected"], 2)
        self.assertEqual(self.engine.complete(), ("/погода ", 8))
        self.assertEqual(self.engine.complete(0), ("/say ", 5))
        self.assertFalse(self.engine.dismiss()["active"])
        self.assertFalse(self.view("/")["active"])
        self.assertTrue(self.view("/s")["active"])

    def test_arguments_show_usage_choices_and_errors(self):
        view = self.view("/погода ")
        self.assertEqual((view["ghost"], view["error"]), ("<город> [дни]", False))
        self.assertIn("/погода <город> [дни] — Погода в городе", view["footer"])
        view = self.view("/погода Москва ")
        self.assertEqual([item["title"] for item in view["items"]], ["1", "3", "7"])
        self.assertEqual(self.engine.move(1)["ghost"], "3")
        self.assertEqual(self.engine.complete(), ("/погода Москва 3", 16))
        view = self.view("/погода Москва 5")
        self.assertEqual((view["items"], view["error"]), ([], True))
        self.assertIn("allowed values: 1, 3, 7", view["footer"])
        view = self.view("/погода Москва 3 лишнее")
        self.assertTrue(view["error"])
        view = self.view("/say ")
        self.assertEqual([item["title"] for item in view["items"]], ["true", "false"])
        self.assertEqual(self.view("/say tr")["ghost"], "ue")
        self.assertEqual(self.engine.complete(), ("/say true ", 10))
        self.assertTrue(self.view("/say maybe text")["error"])

    def test_plugin_suggestions_arrive_later_and_stale_ones_are_dropped(self):
        view = self.view("/погода М")
        request = view["request"]
        self.assertEqual((request["name"], request["index"], request["prefix"], request["args"]),
                         ("погода", 0, "М", []))
        self.assertEqual(request["context"], CHAT)
        self.assertNotIn("request", self.view("/погода М"))  # Asked once.
        newer = self.view("/погода Му")["request"]
        self.assertFalse(self.engine.suggestions(request["token"], [{"value": "Москва"}]))
        self.assertFalse(self.engine.suggestions(newer["token"], [{"value": 5}]))
        self.assertFalse(self.engine.suggestions(newer["token"], "Мурманск"))
        self.assertTrue(self.engine.suggestions(newer["token"], [
            {"value": "Мурманск", "description": "город"}, {"value": "Нью-Йорк"}]))
        view = self.engine.view()
        self.assertEqual([(item["title"], item["description"]) for item in view["items"]],
                         [("Мурманск", "город")])
        self.assertEqual(view["ghost"], "рманск")
        self.assertEqual(self.engine.complete(), ("/погода Мурманск ", 17))

    def test_values_with_spaces_are_quoted(self):
        request = self.view("/погода Н")["request"]
        self.engine.suggestions(request["token"], [{"value": "Нижний Новгород"}])
        self.assertEqual(self.engine.complete(), ('/погода "Нижний Новгород" ', 26))
        self.assertEqual(self.engine.submit('/погода "Нижний Новгород" 7', "user")["values"],
                         ["Нижний Новгород", 7])

    def test_submit(self):
        self.assertIsNone(self.engine.submit("/start", "user"))
        self.assertIsNone(self.engine.submit("/ погода", "user"))
        self.assertIsNone(self.engine.submit("/погода Москва\n3", "user"))
        self.assertEqual(self.engine.submit("/weather Москва", "user"), {
            "installation_id": "a" * 32, "name": "погода", "values": ["Москва", 1]})
        self.assertEqual(self.engine.submit("/say yes  two  words ", "user")["values"], [True, "two  words"])
        self.assertIn("Missing argument", self.engine.submit("/погода", "user")["error"])
        self.assertIn("integer", self.engine.submit("/погода Москва три", "user")["error"])
        self.assertIn("Extra argument", self.engine.submit("/погода Москва 3 4", "user")["error"])

    def test_same_name_of_two_plugins_needs_the_plugin(self):
        self.engine.set("b" * 32, "other", [{"name": "say"}])
        view = self.view("/")
        self.assertEqual([item["title"] for item in view["items"]],
                         ["/other:say", "/weather", "/weather:say", "/погода"])
        self.assertIsNone(self.engine.submit("/say true x", "user"))
        self.assertEqual(self.engine.submit("/other:say", "user")["installation_id"], "b" * 32)
        self.engine.remove("b" * 32)
        self.assertEqual(self.engine.submit("/say true x", "user")["installation_id"], "a" * 32)


class CommandPluginTests(unittest.TestCase):
    kind = InProcessManager

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.manager = self.kind(root / "profile")
        self.addCleanup(self.manager.shutdown)
        self.sent = []
        self.manager.sender = self.sent.append
        path = root / "commands.ovyplugin"
        path.write_text(PLUGIN, encoding="utf-8")
        self.identity = self.manager.install(str(path))
        self.manager.enable(self.identity)

    def dispatch(self, action, **parameters):
        return json.loads(self.manager.dispatch(json.dumps({"action": action, **parameters})))["result"]

    def setting(self, key):
        return self.manager.records[self.identity]["settings"].get(key)

    def test_list_suggest_run(self):
        view = self.dispatch("commands_input", text="/", cursor=1, context=CHAT)
        self.assertEqual([item["title"] for item in view["items"]], ["/fail", "/later", "/погода"])
        self.assertEqual(view["items"][2]["plugin"], "commands")
        view = self.dispatch("commands_input", text="/погода М", cursor=9, context=CHAT)
        self.assertNotIn("request", view)
        # The plugin answered inside the request; the client is told to ask again.
        self.assertEqual(self.setting("suggest"), ["М", "user", []])
        self.assertTrue(any(command.get("method") == "commands.refresh" for command in self.sent))
        view = self.dispatch("commands_view")
        self.assertEqual([item["title"] for item in view["items"]], ["Москва", "Мурманск"])
        result = self.dispatch("commands_key", key="down")
        self.assertEqual(result["view"]["selected"], 1)
        result = self.dispatch("commands_key", key="tab")
        self.assertEqual((result["text"], result["cursor"]), ("/погода Мурманск ", 17))
        self.assertEqual([item["title"] for item in result["view"]["items"]], ["1", "3", "7"])
        result = self.dispatch("commands_pick", index=2)
        self.assertEqual(result["text"], "/погода Мурманск 7")
        self.assertEqual(self.dispatch("commands_submit", text=result["text"], context=CHAT),
                         {"handled": True, "error": ""})
        self.assertEqual(self.setting("ran"), ["1", "Мурманск", 7])

    def test_text_of_a_failed_command_is_not_sent(self):
        self.assertEqual(self.dispatch("commands_submit", text="/start", context=CHAT),
                         {"handled": False, "error": ""})
        result = self.dispatch("commands_submit", text="/погода", context=CHAT)
        self.assertTrue(result["handled"])
        self.assertIn("Missing argument", result["error"])
        self.assertIsNone(self.setting("ran"))

    def test_commands_leave_with_removal_and_unload(self):
        self.dispatch("commands_submit", text="/погода remove", context=CHAT)
        self.assertEqual(self.dispatch("commands_submit", text="/погода Москва", context=CHAT),
                         {"handled": False, "error": ""})
        self.assertTrue(self.dispatch("commands_input", text="/fa", cursor=3, context=CHAT)["active"])
        self.manager.disable(self.identity)
        self.assertFalse(self.dispatch("commands_input", text="/fa", cursor=3, context=CHAT)["active"])


if __name__ == "__main__":
    unittest.main()
