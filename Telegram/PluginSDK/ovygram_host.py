"""The plugin host process: runs plugin code for the client.

Started by the client (ovygram_hosts.py) with its options as one JSON
argument. Calls of the client arrive on the standard input, replies and
events leave through the standard output, see ovygram_channel.py.
"""

import itertools
import json
import logging
import os
from pathlib import Path
import sys


def channel_streams():
    """The protocol keeps the original descriptors to itself: print() of a
    plugin goes to the error stream, which the client writes to the log."""
    reader = os.fdopen(os.dup(0), "rb")
    writer = os.fdopen(os.dup(1), "wb")
    os.dup2(2, 1)
    null = os.open(os.devnull, os.O_RDONLY)
    os.dup2(null, 0)
    os.close(null)
    sys.stdout = sys.stderr
    return reader, writer


class ChannelHandler(logging.Handler):
    def __init__(self, channel):
        super().__init__()
        self.channel = channel

    def emit(self, record):
        try:
            self.channel.send({"event": "log", "level": record.levelno, "text": self.format(record)})
        except Exception:
            pass


class Client:
    """The client as the executor sees it."""

    def __init__(self, channel):
        self.channel = channel
        self.calls = itertools.count(1)
        self.logger = logging.Logger("ovygram")
        self.logger.addHandler(ChannelHandler(channel))

    def submit(self, command):
        self.channel.send({"event": "submit", "command": command})

    def report_error(self, installation_id, message):
        self.channel.send({"event": "error", "installation_id": installation_id, "message": message})

    def mask_changed(self, mask):
        self.channel.send({"event": "mask", "value": mask})

    def restart_required(self):
        self.channel.send({"event": "restart_required"})

    def commands_changed(self, installation_id, commands):
        self.channel.send({"event": "commands", "installation_id": installation_id, "commands": commands})

    def command_suggestions(self, token, items):
        self.channel.send({"event": "command_suggestions", "token": token, "items": items})

    def set_setting(self, installation_id, key, value, account_id):
        # Runs on the main thread inside a call of the client, which answers
        # this request before it goes on waiting for the reply to its own.
        identity = next(self.calls)
        self.channel.send({"call": identity, "action": "set_setting", "params": {
            "installation_id": installation_id, "key": key, "value": value, "account_id": account_id}})
        message = self.channel.receive()
        if message.get("reply") != identity:
            raise RuntimeError("Клиент нарушил порядок обмена с процессом плагинов.")
        if "error" in message:
            raise ValueError(message["error"])


def main():
    reader, writer = channel_streams()
    options = json.loads(sys.argv[1])
    sdk = str(Path(__file__).resolve().parent)
    if sdk not in sys.path:
        sys.path.append(sdk)

    from ovygram_channel import Channel, ChannelClosed
    if options.get("scope"):
        from ovygram_sandbox import apply_scope
        apply_scope()
    if options.get("dependencies"):
        # After the standard library and the SDK, as the client used to.
        sys.path.append(options["dependencies"])

    from hook_utils import HookCancelled
    from ovygram_executor import Executor

    channel = Channel(reader, writer)
    executor = Executor(Client(channel))
    if options.get("bridge"):
        executor.runtime.ready.set()
    channel.send({"event": "hello", "scoped": bool(options.get("scope"))})
    while True:
        try:
            message = channel.receive()
        except ChannelClosed:
            break
        identity = message.get("call")
        try:
            reply = {"reply": identity, "result": executor.dispatch(message["action"], message["params"])}
        except HookCancelled as error:
            reply = {"reply": identity, "error": str(error), "cancelled": True}
        except BaseException as error:
            reply = {"reply": identity, "error": str(error) or type(error).__name__}
        try:
            channel.send(reply)
        except ChannelClosed:
            break
        except (TypeError, ValueError) as error:
            # A result that isn't JSON or is too large.
            channel.send({"reply": identity, "error": str(error) or type(error).__name__})
        if message.get("action") == "shutdown":
            break
    # A plugin may have left threads that would keep the process alive.
    sys.stderr.flush()
    os._exit(0)


if __name__ == "__main__":
    main()
