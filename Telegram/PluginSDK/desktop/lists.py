"""Asynchronous readers for Telegram lists. Stop iteration to stop fetching pages."""

from . import tl
from .client import _account, _chat, rpc
from .models import message_from_tl, peer_id


async def participants(chat, *, query="", limit=100):
    """Yield participant TL records in a channel, supergroup or basic group."""
    _chat(chat)
    _limit(limit)
    if chat.kind == "channel":
        peer = chat.input_peer
        channel = tl.inputChannel(channel_id=peer.channel_id, access_hash=peer.access_hash)
        condition = tl.channelParticipantsSearch(q=query) if query else tl.channelParticipantsRecent()
        offset = 0
        while True:
            page = await rpc(chat.account_id, tl.channels.getParticipants(
                channel=channel, filter=condition, offset=offset, limit=limit, hash=0))
            for participant in page.participants:
                yield participant
            offset += len(page.participants)
            if not page.participants or offset >= page.count:
                return
    elif chat.kind == "chat":
        page = await rpc(chat.account_id, tl.messages.getFullChat(chat_id=chat.input_peer.chat_id))
        roster = page.full_chat.participants
        if roster.constructor_name != "chatParticipants":
            raise PermissionError("Telegram не предоставил список участников.")
        users = {user.id: user for user in page.users}
        for participant in roster.participants:
            user = users.get(participant.user_id)
            if query and (user is None or query.casefold() not in " ".join(
                getattr(user, field, "") or "" for field in ("first_name", "last_name", "username")
            ).casefold()):
                continue
            yield participant
    else:
        raise ValueError("Для списка участников нужна группа или канал.")


def _limit(value):
    if type(value) is not int or not 1 <= value <= 100:
        raise ValueError("Размер страницы должен быть от 1 до 100.")


async def shared_media(chat, *, kind="photo_video", limit=50):
    """Yield Message snapshots matching the selected Telegram media filter."""
    _chat(chat)
    _limit(limit)
    filters = {"photo_video": tl.inputMessagesFilterPhotoVideo, "photo": tl.inputMessagesFilterPhotos,
               "video": tl.inputMessagesFilterVideo, "file": tl.inputMessagesFilterDocument,
               "music": tl.inputMessagesFilterMusic, "voice": tl.inputMessagesFilterVoice,
               "url": tl.inputMessagesFilterUrl}
    if kind not in filters:
        raise ValueError("Неизвестный фильтр медиа.")
    offset = 0
    while True:
        page = await rpc(chat.account_id, tl.messages.search(
            peer=chat.input_peer, q="", filter=filters[kind](), min_date=0, max_date=0,
            offset_id=offset, add_offset=0, limit=limit, max_id=0, min_id=0, hash=0))
        if not page.messages:
            return
        for entry in page.messages:
            if entry.constructor_name == "message" and (not offset or entry.id < offset):
                yield message_from_tl(chat.account_id, entry)
        next_offset = page.messages[-1].id
        if next_offset <= 0 or (offset and next_offset >= offset):
            raise RuntimeError("Telegram вернул повторяющуюся страницу медиа.")
        offset = next_offset
        if page.constructor_name == "messages.messages":
            return


def _input_peer(peer, page):
    if peer.constructor_name == "peerChat":
        return tl.inputPeerChat(chat_id=peer.chat_id)
    if peer.constructor_name == "peerUser":
        user = next((u for u in page.users if u.id == peer.user_id), None)
        if user and getattr(user, "self_", False):
            return tl.inputPeerSelf()
        if user and getattr(user, "access_hash", None) is not None:
            return tl.inputPeerUser(user_id=user.id, access_hash=user.access_hash)
    if peer.constructor_name == "peerChannel":
        channel = next((c for c in page.chats if c.id == peer.channel_id), None)
        if channel and getattr(channel, "access_hash", None) is not None:
            return tl.inputPeerChannel(channel_id=channel.id, access_hash=channel.access_hash)
    raise RuntimeError("Telegram не предоставил данные чата для следующей страницы.")


async def dialogs(account, *, folder_id=None, limit=50):
    """Yield raw dialog TL records. Telegram folder summary rows are omitted."""
    account = _account(account)
    _limit(limit)
    offset_id = offset_date = 0
    offset_peer = tl.inputPeerEmpty()
    seen = set()
    while True:
        page = await rpc(account, tl.messages.getDialogs(folder_id=folder_id, offset_id=offset_id,
                         offset_date=offset_date, offset_peer=offset_peer,
                         exclude_pinned=bool(offset_id), limit=limit, hash=0))
        for dialog in page.dialogs:
            if dialog.constructor_name != "dialog":
                continue
            identity = peer_id(dialog.peer)
            if identity not in seen:
                seen.add(identity)
                yield dialog
        if not page.dialogs or page.constructor_name == "messages.dialogs":
            return
        messages = {(peer_id(m.peer_id), m.id): m for m in page.messages
                    if getattr(m, "peer_id", None) is not None and getattr(m, "date", 0)}
        tail = next((d for d in reversed(page.dialogs) if d.constructor_name == "dialog"
                     and (peer_id(d.peer), d.top_message) in messages), None)
        if tail is None:
            raise RuntimeError("В ответе нет курсора для продолжения списка чатов.")
        message = messages[peer_id(tail.peer), tail.top_message]
        next_peer = _input_peer(tail.peer, page)
        if offset_id == message.id and offset_date == message.date and tl.encode(next_peer) == tl.encode(offset_peer):
            raise RuntimeError("Telegram повторил страницу чатов.")
        offset_id, offset_date, offset_peer = message.id, message.date, next_peer


def _hide(chat, kind, ids):
    from ._runtime import context
    from .ui import WidgetRule

    _chat(chat)
    selected = set()
    for identity in ids:
        if type(identity) is not int or not 0 < identity < 2**64:
            raise ValueError("Ожидаются положительные целочисленные ID.")
        selected.add(identity)
        if len(selected) > 256:
            raise ValueError("Скрыть можно до 256 элементов за одно правило.")
    if not selected:
        raise ValueError("Выбери хотя бы один элемент списка.")
    return WidgetRule(context(), "ui.list_visibility.add", list=kind,
                      account_id=chat.account_id, chat_id=str(chat.id),
                      ids=[str(identity) for identity in sorted(selected)])


def hide_members(chat, peer_ids):
    """Hide matching peers from this chat's member screens."""
    return _hide(chat, "members", peer_ids)


def hide_shared_media(chat, message_ids):
    """Hide messages from this chat's shared media display."""
    return _hide(chat, "media", message_ids)
