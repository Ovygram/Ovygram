"""Messages between the client and the plugin host process.

A message is a JSON object behind its 4-byte big-endian length. Each side
reads the other as untrusted input: a length over the limit or anything but
an object closes the channel.
"""

import json
import struct
import threading

MAX_MESSAGE = 64 * 1024 * 1024
_HEADER = struct.Struct(">I")


class ChannelClosed(Exception):
    pass


class Channel:
    def __init__(self, reader, writer):
        self.reader = reader
        self.writer = writer
        self.lock = threading.Lock()

    def send(self, message):
        data = json.dumps(message, ensure_ascii=False).encode("utf-8")
        if len(data) > MAX_MESSAGE:
            raise ValueError("Слишком большое сообщение для процесса плагинов.")
        try:
            with self.lock:
                self.writer.write(_HEADER.pack(len(data)))
                self.writer.write(data)
                self.writer.flush()
        except (OSError, ValueError) as error:
            raise ChannelClosed(str(error)) from error

    def read(self, size):
        chunks = []
        while size:
            try:
                chunk = self.reader.read(size)
            except (OSError, ValueError) as error:
                raise ChannelClosed(str(error)) from error
            if not chunk:
                raise ChannelClosed("Канал закрыт.")
            chunks.append(chunk)
            size -= len(chunk)
        return b"".join(chunks)

    def receive(self):
        size, = _HEADER.unpack(self.read(_HEADER.size))
        if size > MAX_MESSAGE:
            raise ChannelClosed("Сообщение превышает допустимый размер.")
        try:
            message = json.loads(self.read(size))
        except ValueError as error:
            raise ChannelClosed("Сообщение не является JSON.") from error
        if not isinstance(message, dict):
            raise ChannelClosed("Сообщение не является объектом.")
        return message
