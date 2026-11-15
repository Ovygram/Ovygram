"""Background preparation/checking, followed by main-thread commits.

Workers only read immutable snapshots and return data. They never execute a
plugin, modify installations or call Qt. Prepared packages retain the exact
bytes whose hash and metadata the confirmation dialog displays.
"""

import copy
from datetime import datetime
import hashlib
import json
import queue
import threading
import time
import uuid

from ovygram_package import IncompatiblePlugin, parse_version, version_follows
from ovygram_registry import Registry, TRUSTED, protected_author


class Distribution:
    def __init__(self, manager, registry=None):
        self.manager = manager
        self.registry = registry or Registry()
        self.completed = queue.SimpleQueue()
        self.jobs = {}
        self.offers = {}
        self.notices = []
        self.closed = False
        self.checking = False
        self.last_check = None
        saved = self.manager.distribution_state.get("offers", {})
        if isinstance(saved, dict):
            for identity, offer in saved.items():
                record = self.manager.records.get(identity)
                if not record or not isinstance(offer, dict) or type(offer.get("rollback")) is not bool \
                        or offer.get("from_directory") != record["current"]["directory"]:
                    continue
                trust = self.registry.cached(record["current"])
                sequence = offer.get("release_sequence")
                if type(sequence) is not int or type(offer.get("chain")) is not int \
                        or offer["chain"] != trust.get("chain") or not trust.get("release_sequence"):
                    continue
                if offer.get("rollback"):
                    valid = trust["status"] in ("blocked", "revoked") and 0 < sequence < trust["release_sequence"]
                else:
                    valid = trust["status"] in TRUSTED and record["current"]["metadata"]["release"] \
                        and sequence > trust["release_sequence"]
                if valid:
                    self.offers[identity] = copy.deepcopy(offer)

    def submit(self, operation, work, context=None):
        self.prune()
        if len(self.jobs) >= 16:
            raise ValueError("Дождитесь завершения предыдущей проверки.")
        token = uuid.uuid4().hex
        self.jobs[token] = {"operation": operation, "context": context, "created": time.monotonic(),
                            "state": "pending"}

        def run():
            try:
                result, error = work(), None
            except Exception as exception:
                result, error = None, str(exception) or type(exception).__name__
            if not self.closed:
                self.completed.put((token, result, error))

        threading.Thread(target=run, name="ovygram-registry", daemon=True).start()
        return token

    def prune(self):
        for token, job in tuple(self.jobs.items()):
            if job["state"] != "pending" and time.monotonic() - job["created"] > 900:
                self.jobs.pop(token)

    def prepare(self, path):
        # Reading/parsing/hashing happens in the worker too, with bounded reads.
        def work():
            payload = self.manager.read_file(path)
            return self.package(payload, path)
        return self.submit("prepare", work)

    def package(self, payload, filename):
        from ovygram_runtime import read_package
        metadata, kind = read_package(payload, filename)
        digest = hashlib.sha256(payload).hexdigest()
        trust = self.registry.check(metadata, digest, kind)
        if trust["status"] == "verification_unavailable":
            self.manager.logger.warning("Registry check for %s: %s", metadata["id"], trust.get("reason"))
        return {"payload": payload, "metadata": metadata, "package_type": kind, "sha256": digest,
                "trust": trust}

    def prepared(self, token):
        self.poll()
        job = self.jobs[token]
        if job["state"] != "ready" or job["operation"] != "prepare":
            return {"state": job["state"], "error": job.get("error", "")}
        result = job["result"]
        trust = self.registry.cached({**result, "blocked": result["trust"]["status"] == "blocked"})
        hidden = protected_author(result["metadata"]["author"], trust["status"])
        metadata = {**result["metadata"], "author_hidden": hidden,
                    "display_author": "Неизвестный автор" if hidden else result["metadata"]["author"]}
        return {"state": "ready", "token": token, "metadata": metadata,
                "package_type": result["package_type"], "sha256": result["sha256"], "trust": trust,
                "installations": [self.manager.describe(identity) for identity in
                                  self.manager.installations_of(result["metadata"]["id"])]}

    def commit(self, token, installation_id="", separate=False, allow_unavailable=False,
               allow_loss=False):
        self.poll()
        job = self.jobs[token]
        if job["state"] != "ready" or job["operation"] != "prepare":
            raise ValueError("Файл ещё не проверен.")
        package = job["result"]
        trust = self.registry.cached({**package, "blocked": package["trust"]["status"] == "blocked"})
        if trust["status"] == "verification_unavailable" and not allow_unavailable:
            raise ValueError("Требуется отдельное подтверждение недоступности реестра.")
        result = self.manager.install_bytes(package["payload"], package["metadata"],
                                            package["package_type"], installation_id, separate,
                                            trust, allow_loss=allow_loss)
        if result in self.offers:
            self.offers.pop(result)
            self.manager.distribution_state["offers"] = copy.deepcopy(self.offers)
            self.manager.save()
        self.jobs.pop(token)
        return result

    def discard(self, token):
        self.jobs.pop(token, None)

    def start_check(self, manual=False, now=None):
        if self.checking:
            return False
        now = now or datetime.now().astimezone()
        interval = f"{now.date()}:{now.hour // 12}"
        if not manual and self.manager.distribution_state.get("interval") == interval:
            return False
        records = copy.deepcopy(self.manager.records)
        # Keep the last attempted interval across restarts, even on network errors.
        self.manager.distribution_state["interval"] = interval
        self.manager.save()
        self.checking = True

        def work():
            checked, offers, errors = {}, {}, []
            # Status refresh must work even if GitHub is unavailable.
            for identity, record in records.items():
                version = record["current"]
                if not version.get("sha256"):
                    continue
                trust = self.registry.check(version["metadata"], version["sha256"], version["package_type"])
                checked[identity] = {"directory": version["directory"], "trust": trust}
                if trust["status"] == "verification_unavailable":
                    errors.append(record["plugin_id"] + ": " + (trust.get("reason") or "Проверка реестра недоступна."))
                old = version.get("trust", {})
                chain = trust.get("chain") or old.get("chain")
                sequence = trust.get("release_sequence") or old.get("release_sequence")
                if trust["status"] in ("blocked", "revoked") and chain and sequence:
                    local = record.get("previous")
                    if local and local.get("sha256"):
                        try:
                            self.manager.check_integrity(local)
                            candidate = self.registry.check(local["metadata"], local["sha256"], local["package_type"])
                            if candidate["status"] in TRUSTED and candidate.get("chain") == chain \
                                    and candidate["release_sequence"] < sequence:
                                offers[identity] = {**candidate, "rollback": True,
                                                    "local_directory": local["directory"],
                                                    "from_directory": version["directory"]}
                        except Exception as error:
                            errors.append(str(error))
                    try:
                        safe = [candidate for candidate in self.registry.chain(chain, record["plugin_id"])
                                if candidate.get("status") in TRUSTED
                                and candidate.get("publication") == "published" and candidate.get("url")
                                and type(candidate.get("release_sequence")) is int
                                and candidate["release_sequence"] < sequence]
                        if safe:
                            candidate = max(safe, key=lambda item: item["release_sequence"])
                            if candidate["release_sequence"] > offers.get(identity, {}).get("release_sequence", 0):
                                offers[identity] = {**candidate, "rollback": True, "chain": chain,
                                                    "from_directory": version["directory"]}
                    except Exception as error:
                        errors.append(str(error))
            try:
                index = self.registry.index()
                for identity, record in records.items():
                    version = record["current"]
                    trust = checked.get(identity, {}).get("trust", {})
                    candidate = index.get(record["plugin_id"], {})
                    sequence = candidate.get("release_sequence")
                    if trust.get("status") in TRUSTED and version["metadata"]["release"] \
                            and type(sequence) is int and sequence > trust["release_sequence"]:
                        offers[identity] = {**candidate, "chain": trust["chain"], "rollback": False,
                                            "from_directory": version["directory"]}
            except Exception as error:
                errors.append(str(error))
            return {"checked": checked, "offers": offers, "errors": errors}

        try:
            self.submit("check", work)
        except Exception:
            self.checking = False
            raise
        return True

    def apply_offer(self, installation_id):
        offer = copy.deepcopy(self.offer_for(installation_id))
        if offer is None:
            raise ValueError("Предложение обновления устарело. Повторите проверку.")
        record = copy.deepcopy(self.manager.records[installation_id])
        current = record["current"]
        if current["directory"] != offer["from_directory"]:
            raise ValueError("Установленная версия уже изменилась. Повторите проверку.")
        self.manager.check_integrity(current)

        def work():
            # Recheck the source and target on this action, never trust the index.
            source = self.registry.check(current["metadata"], current["sha256"], current["package_type"])
            if source.get("chain") != offer["chain"] or (not offer["rollback"]
                    and (source["status"] not in TRUSTED or not current["metadata"]["release"])):
                raise ValueError("Исходная версия не подтверждена для этой цепочки.")
            def candidate_package(candidate):
                if candidate.get("local_directory"):
                    previous = record.get("previous")
                    if not previous or previous["directory"] != candidate["local_directory"]:
                        raise ValueError("Сохранённая версия для отката изменилась.")
                    self.manager.check_integrity(previous)
                    payload = self.manager.original(previous).read_bytes()
                    suffix = previous["package_type"]
                else:
                    url = candidate["url"]
                    suffix = url.split("?", 1)[0].rsplit(".", 1)[-1]
                    payload = self.registry.package(url)
                if suffix not in ("ovyplugin", "ovyzip"):
                    raise ValueError("Недопустимый формат обновления.")
                package = self.package(payload, f"{record['plugin_id']}.{suffix}")
                metadata, trust = package["metadata"], package["trust"]
                target_sequence = trust.get("release_sequence")
                expected_version = candidate.get("latest_version", candidate.get("version"))
                if metadata["id"] != record["plugin_id"] or trust["status"] not in TRUSTED \
                        or trust.get("chain") != source.get("chain") \
                        or target_sequence != candidate["release_sequence"] or metadata["version"] != expected_version:
                    raise ValueError("Файл обновления не подтверждён сервером для этой версии и автора.")
                if candidate.get("sha256") and package["sha256"] != candidate["sha256"]:
                    raise ValueError("SHA-256 скачанного файла не совпадает с утверждённой версией.")
                if offer["rollback"]:
                    if target_sequence >= source["release_sequence"]:
                        raise ValueError("Это не предыдущая безопасная версия.")
                elif not metadata["release"] or trust.get("publication") != "published" \
                        or not trust.get("url"):
                    raise ValueError("Публикация обновления не подтверждена сервером.")
                elif target_sequence <= source["release_sequence"] or not version_follows(
                        parse_version(metadata["version"]), parse_version(current["metadata"]["version"])):
                    raise ValueError("Версия обновления меньше установленной.")
                return package

            try:
                return candidate_package(offer)
            except IncompatiblePlugin:
                if offer["rollback"] or not offer.get("info_url"):
                    raise
            alternatives = self.registry.info(offer["info_url"], record["plugin_id"])
            alternatives = sorted((candidate for candidate in alternatives if isinstance(candidate, dict)
                                   and type(candidate.get("release_sequence")) is int
                                   and source["release_sequence"] < candidate["release_sequence"] < offer["release_sequence"]),
                                  key=lambda candidate: candidate["release_sequence"], reverse=True)
            for candidate in alternatives:
                try:
                    return candidate_package(candidate)
                except IncompatiblePlugin:
                    continue
            raise ValueError("Нет совместимого опубликованного обновления для текущего Desktop SDK.")

        return self.submit("update", work, {"installation_id": installation_id,
                                            "directory": current["directory"]})

    def poll(self):
        self.manager.check_thread()
        if self.closed:
            return
        while not self.completed.empty():
            token, result, error = self.completed.get()
            job = self.jobs.get(token)
            if job is None:
                continue
            job.update(state="error" if error else "ready", result=result, error=error)
            operation = job["operation"]
            if operation == "prepare":
                continue
            try:
                if error:
                    raise ValueError(error)
                if operation == "check":
                    self.apply_check(result)
                elif operation == "update":
                    context = job["context"]
                    identity = context["installation_id"]
                    record = self.manager.records.get(identity)
                    if record is None or record["current"]["directory"] != context["directory"]:
                        raise ValueError("Экземпляр удалён или обновлён во время загрузки.")
                    if self.registry.cached(result)["status"] not in TRUSTED:
                        raise ValueError("Подтверждение обновления устарело. Повторите проверку.")
                    self.manager.activate_update(identity, result)
                    self.offers.pop(identity, None)
                    self.manager.distribution_state["offers"] = copy.deepcopy(self.offers)
                    self.manager.save()
                    self.notices.append("Плагин обновлён: " + record["plugin_id"])
            except Exception as exception:
                self.notices.append(str(exception))
                if operation == "check":
                    self.last_check = {"checked": 0, "updates": 0, "errors": [str(exception)]}
            finally:
                if operation == "check":
                    self.checking = False
                self.jobs.pop(token, None)

    def apply_check(self, result):
        for identity, checked in result["checked"].items():
            record = self.manager.records.get(identity)
            if record is None or record["current"]["directory"] != checked["directory"]:
                continue
            version = record["current"]
            old_status = version.get("trust", {}).get("status")
            trust = checked["trust"]
            version["verification_unavailable"] = trust["status"] == "verification_unavailable"
            if trust["status"] == "verification_unavailable":
                version["verification_failure"] = copy.deepcopy(trust)
                self.manager.logger.warning("Registry check for %s: %s", record["plugin_id"], trust.get("reason"))
            else:
                version.pop("verification_failure", None)
            if trust["status"] != "verification_unavailable":
                version["trust"] = trust
                version["blocked"] = trust["status"] == "blocked"
            if version.get("blocked"):
                try:
                    self.manager.disable(identity)
                except Exception as error:
                    self.notices.append(str(error))
            if trust["status"] in ("blocked", "revoked") and trust["status"] != old_status:
                self.notices.append(record["plugin_id"] + (": опасный плагин отключён." if
                                    trust["status"] == "blocked" else ": проверка отозвана.")
                                    + (" Доступен безопасный откат в разделе «Плагины»." if
                                       identity in result["offers"] else ""))
        self.offers = {identity: offer for identity, offer in result["offers"].items()
                       if identity in self.manager.records and self.manager.records[identity]["current"]["directory"]
                       == offer["from_directory"]}
        self.notices.extend(result["errors"])
        self.last_check = {"checked": len(result["checked"]), "updates": len(self.offers),
                           "errors": list(result["errors"])}
        self.manager.distribution_state["offers"] = copy.deepcopy(self.offers)
        self.manager.save()

    @staticmethod
    def offer_key(identity, offer):
        return json.dumps([identity, offer.get("chain"), offer.get("release_sequence"),
                           bool(offer.get("rollback"))], separators=(",", ":"))

    def offer_for(self, identity):
        record, offer = self.manager.records.get(identity), self.offers.get(identity)
        return offer if record and offer and record["current"]["directory"] == offer["from_directory"] else None

    def notification(self):
        dismissed = self.manager.distribution_state.get("dismissed_updates", {})
        if not isinstance(dismissed, dict):
            dismissed = {}
        keys = sorted(self.offer_key(identity, offer) for identity, offer in self.offers.items()
                      if identity in self.manager.records
                      and self.manager.records[identity]["current"]["directory"] == offer["from_directory"]
                      and dismissed.get(identity) != self.offer_key(identity, offer))
        return {"count": len(keys), "key": hashlib.sha256("\n".join(keys).encode()).hexdigest() if keys else ""}

    def dismiss_notification(self):
        self.manager.check_thread()
        dismissed = self.manager.distribution_state.get("dismissed_updates", {})
        dismissed = dict(dismissed) if isinstance(dismissed, dict) else {}
        dismissed.update({identity: self.offer_key(identity, offer) for identity, offer in self.offers.items()})
        self.manager.distribution_state["dismissed_updates"] = {
            identity: key for identity, key in dismissed.items() if identity in self.manager.records}
        self.manager.save()

    def status(self):
        return {"checking": self.checking, "busy": bool(self.jobs), "notices": self.notices[-20:],
                "configured": bool(self.registry.url and self.registry.keys),
                "last_check": copy.deepcopy(self.last_check), "notification": self.notification()}

    def close(self):
        self.closed = True
        self.jobs.clear()
