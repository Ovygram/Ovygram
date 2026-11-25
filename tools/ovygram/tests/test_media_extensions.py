import asyncio
import base64
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "Telegram/PluginSDK"))

from desktop import media, tl
from desktop.models import Chat


class MediaExtensionTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)
        self.path = self.directory / "data.bin"
        self.path.write_bytes(b"media")
        self.chat = Chat("10", 1, "Saved", tl.inputPeerSelf(), "user")

    async def test_single_media_types_survive_tl_encoding(self):
        calls = []

        async def rpc(account, request):
            self.assertEqual(account, "10")
            tl.encode(request)
            calls.append(request)
            return True

        with patch.object(media, "rpc", rpc):
            await media.send_photo(self.chat, self.path, caption="cap")
            await media.send_voice(self.chat, self.path, duration=6)
            await media.send_video_note(self.chat, self.path, duration=1.5, size=240)
        photo, voice, note = [r for r in calls if r.constructor_name == "messages.sendMedia"]
        self.assertEqual(photo.media.constructor_name, "inputMediaUploadedPhoto")
        self.assertEqual(photo.message, "cap")
        self.assertTrue(voice.media.attributes[0].voice)
        self.assertTrue(note.media.attributes[0].round_message)
        self.assertEqual(note.media.attributes[0].w, 240)

    async def test_album_uses_uploaded_references_and_one_final_rpc(self):
        groups = []

        async def rpc(account, request):
            self.assertEqual(account, "10")
            tl.encode(request)
            if request.constructor_name == "messages.uploadMedia":
                ref = SimpleNamespace(id=13, access_hash=8, file_reference=b"ref")
                return SimpleNamespace(photo=ref, document=ref)
            if request.constructor_name == "messages.sendMultiMedia":
                groups.append(request.multi_media)
            return True

        items = [media.AlbumItem(self.path, caption="first"),
                 media.AlbumItem(self.path, "video", "second", 5, 320, 200)]
        with patch.object(media, "rpc", rpc):
            await media.send_album(self.chat, items, silent=True)
        self.assertEqual(len(groups), 1)
        self.assertEqual([m.message for m in groups[0]], ["first", "second"])
        self.assertEqual([m.media.constructor_name for m in groups[0]], ["inputMediaPhoto", "inputMediaDocument"])

    async def test_invalid_album_fails_before_upload(self):
        rpc = AsyncMock()
        items = [media.AlbumItem(self.path), media.AlbumItem(self.path, "video")]
        with patch.object(media, "rpc", rpc), self.assertRaises(ValueError):
            await media.send_album(self.chat, items)
        rpc.assert_not_called()

    async def test_pick_file_is_a_private_copy_and_releases_native_grant(self):
        calls = []

        async def request(method, account, **params):
            self.assertEqual(account, "10")
            calls.append(method)
            if method == "media.pick":
                return {"token": "grant", "name": "../file.dat", "size": 4}
            self.assertEqual(params["token"], "grant")
            return base64.b64encode(b"ok").decode() if method == "media.read" else True

        with patch.object(media, "_request", request), patch("file_utils.get_plugin_data_dir", return_value=self.directory):
            copy = await media.pick_file("10")
        self.assertEqual(copy.parent, self.directory / "imports")
        self.assertEqual(copy.read_bytes(), b"okok")
        self.assertEqual(calls, ["media.pick", "media.read", "media.read", "media.release"])

    async def test_cancelled_copy_discards_partial(self):
        request = AsyncMock(side_effect=[{"token": "g", "name": "f", "size": 2},
                                         asyncio.CancelledError(), True])
        with patch.object(media, "_request", request), patch("file_utils.get_plugin_data_dir", return_value=self.directory):
            with self.assertRaises(asyncio.CancelledError):
                await media.pick_file("10")
        self.assertEqual(list((self.directory / "imports").iterdir()), [])
        self.assertEqual(request.await_args.args, ("media.release", "10"))

    async def test_seek_sends_track_identity(self):
        request = AsyncMock(return_value=True)
        state = {"kind": "song", "document_id": "101", "chat_id": "5", "message_id": 71}
        with patch.object(media, "_request", request):
            await media.control_playback("10", state, "seek", position_ms=3200)
        request.assert_awaited_once_with("media.control", "10", **state, action="seek", position_ms=3200)

    async def test_download_file_continues_after_short_chunk_when_size_known(self):
        from desktop.models import Attachment
        chunks = [b"a" * 100, b"b" * 100]
        offsets = []

        async def rpc(account, request, dc_id=None):
            self.assertEqual(account, "10")
            self.assertEqual(dc_id, 2)
            offsets.append(request.offset)
            data = chunks.pop(0) if chunks else b""
            return SimpleNamespace(constructor_name="upload.file", bytes=data)

        location = tl.inputDocumentFileLocation(id=42, access_hash=7, file_reference=b"ref", thumb_size="")
        attachment = Attachment("10", location, 2, 200, "sample.bin")
        destination = self.directory / "downloaded.bin"
        with patch.object(media, "rpc", rpc):
            result = await media.download_file(attachment, destination)
        self.assertEqual(result, destination.resolve())
        self.assertEqual(offsets, [0, 100])
        self.assertEqual(destination.read_bytes(), b"a" * 100 + b"b" * 100)

    def test_safe_file_name_guards_windows_reserved_and_long_utf8(self):
        from desktop.models import safe_file_name
        self.assertEqual(safe_file_name("CON.txt", "file"), "_CON.txt")
        self.assertEqual(safe_file_name("nul", "file"), "_nul")
        self.assertEqual(safe_file_name("com1.log", "file"), "_com1.log")
        long_cyrillic = "привет" * 40 + ".pdf"
        safe = safe_file_name(long_cyrillic, "file")
        self.assertTrue(safe.endswith(".pdf"))
        self.assertLessEqual(len(safe.encode("utf-8")), 180)
