import asyncio
from pathlib import Path
import sys
import threading
import types
import unittest
from unittest.mock import AsyncMock, Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "Telegram/PluginSDK"))

from desktop import actions, ui
from desktop._runtime import _current, Runtime


class ActionTests(unittest.TestCase):
    def setUp(self):
        self.owner = types.SimpleNamespace(active=True, installation_id="a" * 32)
        self.runtime = types.SimpleNamespace(
            lock=threading.RLock(), widget_rules={}, request=AsyncMock(return_value=True),
            submit_ui=Mock(), call=lambda owner, callback, *args: callback(*args))
        self.owner.manager = types.SimpleNamespace(runtime=self.runtime)
        token = _current.set(self.owner)
        self.addCleanup(_current.reset, token)

    def register(self, callback, **kwargs):
        return asyncio.run(actions.register("bookmark", "Закладка", callback, **kwargs))

    def event(self, **fields):
        Runtime.widget_rule_event(self.runtime, fields)

    def test_invoke_from_registered_sources_and_cleanup(self):
        calls = []
        action = self.register(calls.append, tray=True, global_shortcut=True, link=True,
                               shortcut="Ctrl+Shift+B")
        self.assertTrue(action.link.endswith("a" * 32 + "&name=bookmark"))
        params = self.runtime.request.await_args.kwargs
        self.assertEqual(params["shortcut"], "Ctrl+Shift+B")
        self.assertTrue(params["tray"] and params["link"] and params["global_shortcut"])
        key = params["rule_id"]
        event = dict(rule_id=key, installation_id=self.owner.installation_id)
        self.event(**event, value={"source": "link", "account_id": "15"})
        self.event(**event)
        action()
        self.assertEqual(calls, [actions.Invocation("link", "15"), actions.Invocation("shortcut"),
                                 actions.Invocation("button")])
        self.event(rule_id=key, installation_id="other", value={"source": "tray"})
        self.assertEqual(len(calls), 3)
        binding = action.add_menu("main")
        self.event(rule_id=binding.id, installation_id=self.owner.installation_id,
                   value={"account_id": "3"})
        self.assertEqual(calls[-1], actions.Invocation("main", "3"))
        action.remove()
        action.remove()
        self.assertEqual(len(calls), 4)
        self.assertFalse(self.runtime.widget_rules)
        self.event(**event, value={"source": "tray"})
        with self.assertRaises(ui.ObjectDestroyed):
            action()
        self.assertEqual(len(calls), 4)

    def test_rejected_or_cancelled_registration_cleans_up(self):
        self.runtime.request.side_effect = RuntimeError("shortcut conflict")
        with self.assertRaisesRegex(RuntimeError, "conflict"):
            self.register(Mock(), shortcut="Ctrl+Shift+B")
        self.assertFalse(self.runtime.widget_rules)
        self.assertEqual(self.runtime.submit_ui.call_args.args[1], "ui.action.remove")

    def test_action_name_cannot_change_link_query(self):
        with self.assertRaises(ValueError):
            asyncio.run(actions.register("test&name=b", "x", Mock()))
        self.runtime.request.assert_not_awaited()

    def test_callback_blocked_after_unload(self):
        callback = Mock()
        action = self.register(callback)
        self.owner.active = False
        self.event(rule_id=action._id, installation_id=self.owner.installation_id,
                   value={"source": "global_shortcut"})
        callback.assert_not_called()
