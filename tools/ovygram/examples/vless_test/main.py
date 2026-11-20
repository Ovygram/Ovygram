"""Session-only VLESS profiles for Ovygram Desktop SDK 3."""
import asyncio
import base64
import binascii
import json
import secrets
import socket
import sys
from urllib.parse import parse_qsl, unquote, urlsplit
from urllib.request import HTTPRedirectHandler, build_opener
from uuid import UUID

from base_plugin import BasePlugin
from desktop import components, ui
from ui.settings import Button, Header

__id__ = "vless_test"
__name__ = "VLESS для Ovygram"
__description__ = "Тестовый прокси с Xray. Импортирует vless:// и HTTPS-подписки на время сеанса."
__author__ = "Ovygram"
__version__ = "0.1.0"
__release__ = False
__platform__ = "desktop"
__desktop_sdk__ = 3
__requirements__ = []


def profile(link):
    if len(link) > 8192 or any(ord(c) < 32 for c in link):
        raise ValueError("Некорректная ссылка.")
    url = urlsplit(link.strip())
    if (url.scheme != "vless" or not url.hostname or not url.port or url.password is not None
            or url.path not in ("", "/") or not url.username):
        raise ValueError("Нужна ссылка vless:// с UUID, адресом и портом.")
    identity = str(UUID(url.username))
    pairs = parse_qsl(url.query, keep_blank_values=True, max_num_fields=32)
    q = dict(pairs)
    allowed = {"type", "security", "sni", "fp", "alpn", "pbk", "sid", "spx", "path", "host",
               "serviceName", "authority", "mode", "flow", "encryption", "headerType", "allowInsecure"}
    if (len(pairs) != len(q) or q.keys() - allowed or q.get("headerType", "none") != "none"
            or q.get("allowInsecure", "0").lower() not in ("0", "false")):
        raise ValueError("Параметры этой ссылки не поддерживаются.")
    net, security = q.get("type", "tcp"), q.get("security", "none")
    stream = {"network": net, "security": security}
    sni = q.get("sni") or url.hostname
    if security == "tls":
        tls = stream["tlsSettings"] = {"serverName": sni}
        if q.get("fp"):
            tls["fingerprint"] = q["fp"]
        if q.get("alpn"):
            tls["alpn"] = q["alpn"].split(",")
    elif security == "reality" and q.get("pbk"):
        stream["realitySettings"] = {"serverName": sni, "password": q["pbk"], "shortId": q.get("sid", ""),
                                     "fingerprint": q.get("fp", "chrome"), "spiderX": q.get("spx", "/")}
    elif security != "none":
        raise ValueError("Проверьте параметры TLS или REALITY.")
    if net in ("ws", "httpupgrade", "xhttp", "splithttp"):
        key = "xhttp" if net == "splithttp" else net
        transport = stream[key + "Settings"] = {"path": q.get("path", "/"), "host": q.get("host", "")}
        if key == "xhttp":
            transport["mode"] = q.get("mode", "auto")
    elif net == "grpc":
        stream["grpcSettings"] = {"serviceName": q.get("serviceName", ""), "authority": q.get("authority", ""),
                                  "multiMode": q.get("mode") == "multi"}
    elif net not in ("tcp", "raw"):
        raise ValueError("Этот транспорт пока не поддерживается.")
    return (unquote(url.fragment)[:80] or url.hostname, {
        "protocol": "vless", "settings": {"address": url.hostname, "port": url.port, "id": identity,
                                         "flow": q.get("flow", ""), "encryption": q.get("encryption", "none")},
        "streamSettings": stream})


class HttpsRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urlsplit(newurl).scheme != "https":
            raise ValueError("Подписке нужен HTTPS.")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def subscription(link):
    with build_opener(HttpsRedirect()).open(link, timeout=10) as reply:
        data = reply.read(256 * 1024 + 1)
    if len(data) > 256 * 1024:
        raise ValueError("Подписка слишком большая.")
    if b"vless://" not in data:
        compact = b"".join(data.split())
        data = base64.b64decode(compact + b"=" * (-len(compact) % 4), altchars=b"-_", validate=True)
    profiles = []
    for line in data.decode("utf-8-sig").splitlines():
        if line.strip().startswith("vless://"):
            try:
                profiles.append(profile(line.strip()))
            except ValueError:
                continue
        if len(profiles) == 64:
            break
    if not profiles:
        raise ValueError("В подписке нет поддерживаемых VLESS-профилей.")
    return profiles


async def probe(port, user, password):
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    try:
        writer.write(bytes((5, 1, 2)))
        await writer.drain()
        if await reader.readexactly(2) != bytes((5, 2)):
            return False
        writer.write(bytes((1, len(user))) + user.encode() + bytes((len(password),)) + password.encode())
        await writer.drain()
        return await reader.readexactly(2) == bytes((1, 0))
    finally:
        writer.close()
        await writer.wait_closed()


class VlessPlugin(BasePlugin):
    def on_plugin_load(self):
        self.lock = asyncio.Lock()
        ui.add_menu_item("main", "VLESS · подключение", self.open_import)

    def create_settings(self):
        return [Header("Для импорта откройте «VLESS · подключение» в главном меню."),
                Button("stop", "Остановить Xray", self.stop)]

    def open_import(self, menu):
        link = ""

        def change(text):
            nonlocal link
            link = text.strip()

        async def load():
            if not await components.network_available():
                ui.show_dialog("Песочница плагинов работает без доступа к сети. На Linux нужен Landlock ABI 6 или новее.", "VLESS")
                return
            try:
                if link.startswith("https://"):
                    servers = await asyncio.to_thread(subscription, link)
                else:
                    servers = [profile(link)]
            except (ValueError, OSError, binascii.Error):
                ui.show_dialog("Не удалось прочитать ссылку или подписку. Проверьте адрес и параметры.", "VLESS")
                return
            dialog.close()
            ui.show_dialog(ui.Column(*(ui.Button(name, lambda p=out: self.connect(p))
                                       for name, out in servers)), "Выберите сервер")

        dialog = ui.show_dialog(ui.Column(ui.Input(placeholder="vless:// или https://", on_change=change),
                                          ui.Button("Прочитать", load)), "Импорт VLESS")

    async def stop(self):
        async with self.lock:
            await components.stop()

    async def connect(self, outbound):
        async with self.lock:
            await components.stop()
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            user, password = secrets.token_hex(12), secrets.token_hex(24)
            config = {"log": {"loglevel": "none"}, "inbounds": [
                {"listen": "127.0.0.1", "port": port, "protocol": "socks", "settings": {
                    "auth": "password", "accounts": [{"user": user, "pass": password}]}}],
                "outbounds": [outbound]}
            resource = "bin/xray.exe" if sys.platform == "win32" else "bin/xray"
            try:
                await components.start(resource, ["run", "-format", "json", "-config", "stdin:"],
                                       config=json.dumps(config))
                for _ in range(40):
                    try:
                        if await asyncio.wait_for(probe(port, user, password), 0.5):
                            await components.offer_socks5(port, user, password)
                            ui.show_dialog("В настройках прокси Ovygram откройте «Прокси плагинов» "
                                           "и выберите vless_test.", "Xray готов")
                            return
                    except (OSError, TimeoutError, asyncio.IncompleteReadError):
                        pass
                    if not await components.running():
                        break
                    await asyncio.sleep(0.15)
            except Exception:
                # Link, credentials and engine configuration stay out of diagnostics.
                pass
            await components.stop()
            ui.show_dialog("Не удалось запустить локальный SOCKS5. Проверьте пакет для своей ОС, "
                           "параметры сервера и доступность песочницы.", "VLESS")
