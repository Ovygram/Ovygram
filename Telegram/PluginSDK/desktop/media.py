"""Cancellable file transfers using the client's authenticated RPC sessions."""

import asyncio
import base64
from dataclasses import dataclass
import hashlib
import inspect
import math
import mimetypes
import os
from pathlib import Path
import secrets
import uuid

from . import tl
from .client import _account, _chat, _request, get_chat, get_message, rpc
from ._runtime import RpcError
from .models import Attachment, Progress, safe_file_name


PART_SIZE = 512 * 1024


async def _progress(callback, account_id, amount, total):
    if callback is not None:
        result = callback(Progress(account_id, amount, total))
        if inspect.isawaitable(result):
            await result


async def _upload(chat, path, progress):
    _chat(chat)
    path = Path(path).resolve(strict=True)
    size = path.stat().st_size
    if size <= 0:
        raise ValueError("Пустой файл нельзя отправить.")
    identity = secrets.randbits(63)
    total = (size + PART_SIZE - 1) // PART_SIZE
    if total > 8000:
        raise ValueError("Файл превышает предел поддерживаемой загрузки.")
    big = size > 10 * 1024 * 1024
    checksum = hashlib.md5(usedforsecurity=False)
    stream = await asyncio.to_thread(path.open, "rb")
    try:
        for part in range(total):
            data = await asyncio.to_thread(stream.read, PART_SIZE)
            if not data or (part != total - 1 and len(data) != PART_SIZE):
                raise RuntimeError("Файл изменился во время загрузки.")
            if big:
                request = tl.upload.saveBigFilePart(file_id=identity, file_part=part,
                                                    file_total_parts=total, bytes=data)
            else:
                checksum.update(data)
                request = tl.upload.saveFilePart(file_id=identity, file_part=part, bytes=data)
            if not await rpc(chat.account_id, request):
                raise RuntimeError("Telegram не принял часть файла.")
            await _progress(progress, chat.account_id, min((part + 1) * PART_SIZE, size), size)
        if stream.tell() != size or path.stat().st_size != size:
            raise RuntimeError("Файл изменился во время загрузки.")
    finally:
        await asyncio.to_thread(stream.close)
    uploaded = tl.inputFileBig(id=identity, parts=total, name=path.name) if big else tl.inputFile(
        id=identity, parts=total, name=path.name, md5_checksum=checksum.hexdigest())
    return uploaded


async def _send(chat, media, caption, silent):
    return await rpc(chat.account_id, tl.messages.sendMedia(
        peer=chat.input_peer, media=media, message=caption, random_id=secrets.randbits(63), silent=silent))


async def send_file(chat, path, *, caption="", progress=None, silent=False):
    file = await _upload(chat, path, progress)
    media = tl.inputMediaUploadedDocument(file=file,
        mime_type=mimetypes.guess_type(file.name)[0] or "application/octet-stream",
        attributes=[tl.documentAttributeFilename(file_name=file.name)])
    return await _send(chat, media, caption, silent)


async def send_photo(chat, path, *, caption="", progress=None, silent=False):
    file = await _upload(chat, path, progress)
    return await _send(chat, tl.inputMediaUploadedPhoto(file=file), caption, silent)


async def send_voice(chat, path, *, duration, waveform=None, progress=None, silent=False):
    """Upload an already encoded Ogg/Opus voice message; duration is in seconds."""
    if type(duration) is not int or not 0 < duration < 2**31:
        raise ValueError("duration должен быть положительным целым числом секунд.")
    if waveform is not None and not isinstance(waveform, bytes):
        raise TypeError("waveform должен содержать упакованные байты Telegram.")
    audio = tl.documentAttributeAudio(voice=True, duration=duration, waveform=waveform)
    file = await _upload(chat, path, progress)
    media = tl.inputMediaUploadedDocument(file=file, mime_type="audio/ogg", attributes=[audio])
    return await _send(chat, media, "", silent)


def _video_attribute(duration, width, height, round_message=False):
    if (type(duration) not in (int, float) or not math.isfinite(duration) or duration <= 0
            or any(type(n) is not int or not 0 < n < 2**31 for n in (width, height))):
        raise ValueError("Для видео нужны положительные длительность и размеры.")
    return tl.documentAttributeVideo(duration=duration, w=width, h=height, round_message=round_message)


async def send_video_note(chat, path, *, duration, size, progress=None, silent=False):
    """Send a prepared square MP4. size is the side of the frame in pixels."""
    attribute = _video_attribute(duration, size, size, round_message=True)
    file = await _upload(chat, path, progress)
    media = tl.inputMediaUploadedDocument(file=file, mime_type="video/mp4", attributes=[attribute])
    return await _send(chat, media, "", silent)


@dataclass(frozen=True)
class AlbumItem:
    path: str | Path
    kind: str = "photo"
    caption: str = ""
    duration: float = 0
    width: int = 0
    height: int = 0


async def send_album(chat, items, *, progress=None, silent=False):
    """Send 2–10 photos/videos as one group. Progress is per uploaded file."""
    _chat(chat)
    items = list(items)
    if not 2 <= len(items) <= 10:
        raise ValueError("Альбом должен содержать от 2 до 10 файлов.")
    attributes = []
    for item in items:
        if not isinstance(item, AlbumItem) or item.kind not in ("photo", "video"):
            raise ValueError("Элементы альбома должны быть AlbumItem типа photo или video.")
        attributes.append(_video_attribute(item.duration, item.width, item.height)
                          if item.kind == "video" else None)
    group = []
    for item, attribute in zip(items, attributes):
        file = await _upload(chat, item.path, progress)
        media = (tl.inputMediaUploadedPhoto(file=file) if attribute is None else
                 tl.inputMediaUploadedDocument(file=file, mime_type="video/mp4", attributes=[attribute]))
        result = await rpc(chat.account_id, tl.messages.uploadMedia(peer=chat.input_peer, media=media))
        obj = result.photo if attribute is None else result.document
        ref = dict(id=obj.id, access_hash=obj.access_hash, file_reference=obj.file_reference)
        media_ref = (tl.inputMediaPhoto(id=tl.inputPhoto(**ref)) if attribute is None else
                     tl.inputMediaDocument(id=tl.inputDocument(**ref)))
        group.append(tl.inputSingleMedia(media=media_ref, message=item.caption, random_id=secrets.randbits(63)))
    return await rpc(chat.account_id, tl.messages.sendMultiMedia(
        peer=chat.input_peer, multi_media=group, silent=silent))


async def pick_file(account):
    """Ask the user to share a file and copy it into this installation's account data."""
    from file_utils import get_plugin_data_dir

    account_id = _account(account)
    selected = await _request("media.pick", account_id)
    if selected is None:
        return None
    partial = None
    stream = None
    try:
        directory = await asyncio.to_thread(lambda: get_plugin_data_dir(account_id) / "imports")
        await asyncio.to_thread(directory.mkdir, exist_ok=True)
        name = safe_file_name(selected["name"], "file")
        target = directory / (uuid.uuid4().hex + "-" + name)
        staging = target.with_suffix(target.suffix + ".part")
        stream = await asyncio.to_thread(staging.open, "xb")
        partial = staging
        remaining = selected["size"]
        while remaining:
            payload = await _request("media.read", account_id, token=selected["token"])
            chunk = base64.b64decode(payload, validate=True)
            if not 0 < len(chunk) <= remaining:
                raise ValueError("Неполная копия выбранного файла.")
            await asyncio.to_thread(stream.write, chunk)
            remaining -= len(chunk)
        await asyncio.to_thread(stream.flush)
        await asyncio.to_thread(stream.close)
        stream = None
        await asyncio.to_thread(partial.replace, target)
        partial = None
        return target
    finally:
        if stream is not None:
            await asyncio.to_thread(stream.close)
        if partial is not None:
            await asyncio.to_thread(partial.unlink, missing_ok=True)
        try:
            await _request("media.release", account_id, token=selected["token"])
        except (RpcError, RuntimeError):
            # Logging out or unloading also revokes the native file handle.
            pass


async def downloads(account):
    """Current entries in the native downloads list for this account."""
    return await _request("media.downloads", _account(account))


async def cancel_download(account, entry):
    """Cancel only the selected active native transfer, including its start identity."""
    return await _request("media.cancel_download", _account(account), chat_id=entry["chat_id"],
                          message_id=entry["message_id"], started=entry["started"])


async def playback(account, kind="song"):
    if kind not in ("song", "voice"):
        raise ValueError("kind должен быть song или voice.")
    return await _request("media.playback", _account(account), kind=kind)


async def control_playback(account, state, action, *, position_ms=None):
    """Act on the exact track returned by playback(); a stale selection fails."""
    if action not in ("play", "pause", "stop", "next", "previous", "seek"):
        raise ValueError("Неизвестное действие плеера.")
    if action == "seek" and (type(position_ms) is not int or position_ms < 0):
        raise ValueError("Укажите позицию в миллисекундах.")
    return await _request("media.control", _account(account), action=action, kind=state["kind"],
                          chat_id=state["chat_id"], message_id=state["message_id"],
                          document_id=state["document_id"], position_ms=position_ms)


async def download_file(attachment, destination, *, progress=None):
    if not isinstance(attachment, Attachment):
        raise TypeError("Ожидается Attachment с явным account_id.")
    target = Path(destination).resolve()
    await asyncio.to_thread(target.parent.mkdir, parents=True, exist_ok=True)
    partial = target.with_name(target.name + ".part-" + uuid.uuid4().hex)
    stream = await asyncio.to_thread(partial.open, "xb")
    complete = False
    offset = 0
    refreshed = False
    try:
        while attachment.size <= 0 or offset < attachment.size:
            try:
                result = await rpc(attachment.account_id, tl.upload.getFile(
                    location=attachment.location, offset=offset, limit=PART_SIZE,
                    precise=True, cdn_supported=False), dc_id=attachment.dc_id)
            except RpcError as error:
                if (refreshed or not error.name.startswith("FILE_REFERENCE_")
                        or attachment.message_chat_id is None or attachment.message_id is None):
                    raise
                chat = await get_chat(attachment.account_id, attachment.message_chat_id)
                message = await get_message(chat, attachment.message_id)
                updated = message.attachment if message is not None else None
                same_thumb = (getattr(updated.location, "thumb_size", None)
                              == getattr(attachment.location, "thumb_size", None)) if updated is not None else False
                size_ok = (updated is not None and (attachment.size <= 0 or updated.size == attachment.size))
                if (updated is None or updated.location.id != attachment.location.id
                        or not same_thumb or not size_ok):
                    raise RuntimeError("Вложение сообщения изменилось.") from error
                attachment = updated
                refreshed = True
                continue
            if result.constructor_name != "upload.file":
                raise RuntimeError("Telegram вернул неподдерживаемый тип загрузки.")
            data = result.bytes
            if not data:
                break
            await asyncio.to_thread(stream.write, data)
            offset += len(data)
            await _progress(progress, attachment.account_id, offset, attachment.size)
            if attachment.size <= 0 and len(data) < PART_SIZE:
                break
        if attachment.size > 0 and offset != attachment.size:
            raise RuntimeError("Скачан неполный файл.")
        await asyncio.to_thread(stream.flush)
        await asyncio.to_thread(os.fsync, stream.fileno())
        await asyncio.to_thread(stream.close)
        await asyncio.to_thread(partial.replace, target)
        complete = True
    finally:
        if not stream.closed:
            await asyncio.to_thread(stream.close)
        if not complete:
            await asyncio.to_thread(partial.unlink, missing_ok=True)
    return target
