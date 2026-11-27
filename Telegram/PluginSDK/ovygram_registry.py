"""HTTPS registry transport and validation. Indexes never grant trust.

Only deployment-pinned Ed25519 keys are accepted, including for /chains.
OpenSSL is the same system/bundled libcrypto used by CPython's TLS module;
no plugin dependencies, downloaded keys or executable package code are used.
"""

import base64
import ctypes
import ctypes.util
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import threading
import unicodedata
import urllib.parse
import urllib.request

from ovygram_i18n import tr
from ovygram_package import MAX_PACKAGE

TRUSTED = frozenset(("verified", "official"))
STATUSES = TRUSTED | {"unverified", "revoked", "blocked"}
MAX_CLOCK_SKEW = timedelta(seconds=60)
_EVP_PKEY_ED25519 = 1087
_CRYPTO_LOCK = threading.Lock()
_CRYPTO_CACHE = {}


def _unavailable_text():
    return tr("Failed to verify the plugin", "Не удалось проверить плагин")


def _load_crypto():
    library = None
    if sys.platform == "win32":
        import _ssl
        # Installed CPython keeps OpenSSL in DLLs; the embedded runtime keeps
        # it at its root. In both layouts it is beside the TLS extension.
        library = str(Path(_ssl.__file__).resolve().with_name("libcrypto-3.dll"))
    cache_key = (sys.platform, library, ctypes.CDLL)
    with _CRYPTO_LOCK:
        cached = _CRYPTO_CACHE.get(cache_key)
        if cached is not None:
            return cached
        crypto = None
        if sys.platform == "win32":
            crypto = ctypes.CDLL(library)
        else:
            candidates = ["libcrypto.so.3", "libcrypto.so.1.1", None]
            found = ctypes.util.find_library("crypto")
            if found:
                candidates.append(found)
            for candidate in candidates:
                try:
                    loaded = ctypes.CDLL(candidate)
                except OSError:
                    continue
                if hasattr(loaded, "EVP_PKEY_new_raw_public_key_ex") or hasattr(
                        loaded, "EVP_PKEY_new_raw_public_key"):
                    crypto = loaded
                    break
        if crypto is None:
            raise ValueError(tr("OpenSSL libcrypto is unavailable.", "OpenSSL libcrypto недоступна."))
        has_ex = hasattr(crypto, "EVP_PKEY_new_raw_public_key_ex")
        if has_ex:
            crypto.EVP_PKEY_new_raw_public_key_ex.argtypes = [
                ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
                ctypes.c_char_p, ctypes.c_size_t]
            crypto.EVP_PKEY_new_raw_public_key_ex.restype = ctypes.c_void_p
        else:
            crypto.EVP_PKEY_new_raw_public_key.argtypes = [
                ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
            crypto.EVP_PKEY_new_raw_public_key.restype = ctypes.c_void_p
        crypto.EVP_MD_CTX_new.restype = ctypes.c_void_p
        crypto.EVP_DigestVerifyInit.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
            ctypes.c_void_p, ctypes.c_void_p]
        crypto.EVP_DigestVerify.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t,
            ctypes.c_char_p, ctypes.c_size_t]
        crypto.EVP_MD_CTX_free.argtypes = [ctypes.c_void_p]
        crypto.EVP_PKEY_free.argtypes = [ctypes.c_void_p]
        result = (crypto, has_ex)
        _CRYPTO_CACHE[cache_key] = result
        return result


def canonical(document):
    return json.dumps({key: value for key, value in document.items() if key != "signature"},
                      sort_keys=True, separators=(",", ":"), ensure_ascii=False,
                      allow_nan=False).encode("utf-8")


def verify_ed25519(key, signature, payload):
    if len(key) != 32 or len(signature) != 64:
        raise ValueError(tr("Invalid registry signature.", "Некорректная подпись реестра."))
    crypto, has_ex = _load_crypto()
    public = (crypto.EVP_PKEY_new_raw_public_key_ex(None, b"ED25519", None, key, len(key))
              if has_ex else crypto.EVP_PKEY_new_raw_public_key(_EVP_PKEY_ED25519, None, key, len(key)))
    context = crypto.EVP_MD_CTX_new()
    try:
        if not public or not context or crypto.EVP_DigestVerifyInit(
                context, None, None, None, public) != 1 or crypto.EVP_DigestVerify(
                    context, signature, len(signature), payload, len(payload)) != 1:
            raise ValueError(tr(
                "Registry signature verification failed.",
                "Подпись реестра не прошла проверку.",
            ))
    finally:
        if context:
            crypto.EVP_MD_CTX_free(context)
        if public:
            crypto.EVP_PKEY_free(public)


def https_url(url):
    parts = urllib.parse.urlsplit(url)
    if parts.scheme != "https" or not parts.hostname or parts.username or parts.password or parts.fragment:
        raise ValueError(tr(
            "An HTTPS URL is required for the registry and packages.",
            "Для реестра и пакетов требуется HTTPS URL.",
        ))
    return url


class HttpsRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, url):
        https_url(url)
        return super().redirect_request(request, response, code, message, headers, url)


def download(url, limit):
    https_url(url)
    request = urllib.request.Request(url, headers={"User-Agent": "Ovygram-PluginRegistry/1"})
    with urllib.request.build_opener(HttpsRedirect()).open(request, timeout=15) as response:
        https_url(response.geturl())
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError(tr(
            "Server response exceeds the allowed size.",
            "Ответ сервера превышает допустимый размер.",
        ))
    return data


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(tr("Duplicate JSON field.", "Повторяющееся поле JSON."))
        result[key] = value
    return result


def json_document(payload):
    document = json.loads(payload, object_pairs_hook=unique_object,
                          parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    if not isinstance(document, dict):
        raise ValueError(tr("Expected a JSON object.", "Ожидается объект JSON."))
    return document


def unavailable(reason=None):
    return {"status": "verification_unavailable", "reason": reason or _unavailable_text(),
            "chain": None, "release_sequence": None}


class Registry:
    def __init__(self, config=None, fetch=download, now=None):
        if config is None:
            path = Path(__file__).with_name("registry.json")
            config = json_document(path.read_bytes()) if path.exists() else {}
            config = {**config,
                      "server_url": os.environ.get("OVYGRAM_REGISTRY_URL") or config.get("server_url", ""),
                      "index_url": os.environ.get("OVYGRAM_PLUGIN_INDEX_URL") or config.get("index_url", "")}
        self.url = config.get("server_url", "").rstrip("/")
        self.index_url = config.get("index_url", "")
        self.keys = config.get("public_keys", {})
        self.fetch = fetch
        self.now = now or (lambda: datetime.now(timezone.utc))

    def validate(self, answer):
        key = base64.b64decode(self.keys[answer["key_id"]], validate=True)
        if hashlib.sha256(key).hexdigest()[:16] != answer["key_id"]:
            raise ValueError(tr(
                "Key ID does not match the pinned public key.",
                "ID ключа не совпадает с закреплённым ключом.",
            ))
        verify_ed25519(key, base64.b64decode(answer["signature"], validate=True), canonical(answer))
        issued = datetime.fromisoformat(answer["issued_at"].replace("Z", "+00:00"))
        expires = datetime.fromisoformat(answer["expires_at"].replace("Z", "+00:00"))
        now = self.now()
        # Separate computers do not share a subsecond clock. Accept a small
        # future issue time only; never extend an answer's expiry or TTL.
        if issued.tzinfo is None or expires.tzinfo is None or not issued < expires \
                or issued > now + MAX_CLOCK_SKEW or now >= expires \
                or (expires - issued).total_seconds() > 8 * 86400:
            raise ValueError(tr(
                "Registry response is expired or has an invalid timestamp.",
                "Ответ реестра устарел или имеет некорректное время.",
            ))
        return answer

    def signed(self, path):
        if not self.url or not self.keys:
            raise ValueError(tr(
                "Registry is not configured: URL and pinned public key are required.",
                "Реестр не настроен: требуются URL и закреплённый публичный ключ.",
            ))
        return self.validate(json_document(self.fetch(self.url + path, 2 * 1024 * 1024)))

    def match(self, answer, metadata, digest, kind):
        if (answer.get("plugin_id"), answer.get("sha256"), answer.get("package_type")) != (
                metadata["id"], digest, kind) or answer.get("status") not in STATUSES:
            raise ValueError(tr(
                "Registry response belongs to a different file.",
                "Ответ реестра относится к другому файлу.",
            ))
        if answer["status"] in TRUSTED:
            if answer.get("version") != metadata["version"] \
                    or answer.get("release") is not metadata["release"] \
                    or type(answer.get("chain")) is not int or answer["chain"] < 1 \
                    or type(answer.get("release_sequence")) is not int or answer["release_sequence"] < 1:
                raise ValueError(tr(
                    "Verified metadata does not match the package.",
                    "Подтверждённые метаданные не совпадают с пакетом.",
                ))
            if answer["status"] == "official" and metadata["author"] != "Ovygram":
                raise ValueError(tr(
                    "Invalid verification for official author.",
                    "Некорректное подтверждение официального автора.",
                ))
        return answer

    def check(self, metadata, digest, kind):
        try:
            query = urllib.parse.urlencode({"plugin_id": metadata["id"], "sha256": digest,
                                            "package_type": kind})
            return self.match(self.signed("/v1/check?" + query), metadata, digest, kind)
        except Exception as error:
            return unavailable(str(error))

    def cached(self, version):
        answer = version.get("trust", {})
        if version.get("verification_unavailable") or answer.get("status") == "verification_unavailable":
            failure = version.get("verification_failure") or answer
            result = unavailable(failure.get("reason") or _unavailable_text())
            return {**result, "status": "blocked"} if version.get("blocked") else result
        try:
            return self.match(self.validate(answer), version["metadata"], version["sha256"],
                              version["package_type"])
        except Exception as error:
            # An outage/expiry must never remove a known block.
            result = unavailable(str(error) or _unavailable_text())
            return {**result, "status": "blocked"} if version.get("blocked") else result

    def index(self):
        if not self.index_url:
            return {}
        result = json_document(self.fetch(https_url(self.index_url), 2 * 1024 * 1024))["plugins"]
        if not isinstance(result, dict):
            raise ValueError(tr("Invalid plugin index.", "Некорректный индекс плагинов."))
        return result

    def chain(self, chain, plugin_id):
        versions, cursor = [], 0
        for _ in range(20):
            answer = self.signed(f"/v1/chains/{int(chain)}?limit=500&after={cursor}")
            if answer.get("chain") != chain or answer.get("plugin_id") != plugin_id \
                    or not isinstance(answer.get("versions"), list):
                raise ValueError(tr(
                    "Response belongs to a different author chain.",
                    "Ответ относится к другой цепочке автора.",
                ))
            versions.extend(answer["versions"])
            if len(versions) > 10000:
                break
            next_cursor = answer.get("next_cursor")
            if next_cursor is None:
                return versions
            if type(next_cursor) is not int or next_cursor <= cursor:
                raise ValueError(tr(
                    "Invalid version history cursor.",
                    "Некорректный курсор истории версий.",
                ))
            cursor = next_cursor
        raise ValueError(tr("Version history is too large.", "История версий слишком велика."))

    def info(self, url, plugin_id):
        # Discovery only: every selected package still needs a signed check.
        answer = json_document(self.fetch(https_url(url), 2 * 1024 * 1024))
        if answer.get("id") != plugin_id or not isinstance(answer.get("versions"), list):
            raise ValueError(tr(
                "Version card belongs to a different plugin.",
                "Карточка версий относится к другому плагину.",
            ))
        return answer["versions"]

    def package(self, url):
        return self.fetch(https_url(url), MAX_PACKAGE)


CONFUSABLES = str.maketrans({"о": "o", "ο": "o", "ѵ": "v", "у": "y", "ү": "y",
                            "ɡ": "g", "ԍ": "g", "г": "r", "а": "a", "α": "a",
                            "м": "m", "ｍ": "m", "0": "o"})


def _levenshtein(word, target):
    row = list(range(len(target) + 1))
    for index, character in enumerate(word, 1):
        current = [index]
        for column, expected in enumerate(target, 1):
            current.append(min(current[-1] + 1, row[column] + 1,
                               row[column - 1] + (character != expected)))
        row = current
    return row[-1]


def protected_author(author, status):
    if status in TRUSTED:
        return False
    normalized = unicodedata.normalize("NFKC", author).casefold().translate(CONFUSABLES)
    normalized = "".join(character for character in normalized
                         if unicodedata.category(character)[0] not in ("C", "M"))
    if "ovygram" in normalized:
        return True
    collapsed = re.sub(r"[\W_]+", "", normalized)
    if "ovygram" in collapsed:
        return True
    words = re.findall(r"[^\W_]+", normalized)
    for word in words:
        if 5 <= len(word) <= 9 and _levenshtein(word, "ovygram") <= 2:
            return True
    if len(words) > 1 and 5 <= len(collapsed) <= 9 and _levenshtein(collapsed, "ovygram") <= 1:
        return True
    return False
