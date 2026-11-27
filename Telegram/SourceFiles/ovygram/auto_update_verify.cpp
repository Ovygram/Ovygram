#include "ovygram/auto_update_verify.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonValue>
#include <QtCore/QRegularExpression>

#include <openssl/evp.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace Ovygram {
namespace {

constexpr auto kClockSkewAllowanceSeconds = qint64(300);

void SetError(QString *error, const QString &message) {
	if (error) {
		*error = message;
	}
}

struct PKeyDeleter {
	void operator()(EVP_PKEY *key) const {
		EVP_PKEY_free(key);
	}
};

struct MdCtxDeleter {
	void operator()(EVP_MD_CTX *ctx) const {
		EVP_MD_CTX_free(ctx);
	}
};

bool UnicodeCodePointLess(const QString &a, const QString &b) {
	const auto ua = a.toUcs4();
	const auto ub = b.toUcs4();
	return std::lexicographical_compare(ua.begin(), ua.end(), ub.begin(), ub.end());
}

void AppendJsonString(QByteArray &out, const QString &text) {
	out.append('"');
	const auto utf8 = text.toUtf8();
	for (auto i = 0; i < utf8.size(); ++i) {
		const auto ch = static_cast<unsigned char>(utf8.at(i));
		switch (ch) {
		case '"': out.append("\\\""); break;
		case '\\': out.append("\\\\"); break;
		case '\b': out.append("\\b"); break;
		case '\f': out.append("\\f"); break;
		case '\n': out.append("\\n"); break;
		case '\r': out.append("\\r"); break;
		case '\t': out.append("\\t"); break;
		default:
			if (ch < 0x20) {
				static constexpr char kHex[] = "0123456789abcdef";
				out.append("\\u00");
				out.append(kHex[(ch >> 4) & 0xF]);
				out.append(kHex[ch & 0xF]);
			} else {
				out.append(static_cast<char>(ch));
			}
			break;
		}
	}
	out.append('"');
}

void AppendJsonValue(QByteArray &out, const QJsonValue &value, bool stripTopSignature = false) {
	switch (value.type()) {
	case QJsonValue::Null:
	case QJsonValue::Undefined:
		out.append("null");
		return;
	case QJsonValue::Bool:
		out.append(value.toBool() ? "true" : "false");
		return;
	case QJsonValue::Double: {
		const auto d = value.toDouble();
		const auto asInt = value.toInteger();
		if (std::isfinite(d) && static_cast<double>(asInt) == d) {
			out.append(QByteArray::number(asInt));
		} else {
			out.append(QByteArray::number(d, 'g', 17));
		}
		return;
	}
	case QJsonValue::String:
		AppendJsonString(out, value.toString());
		return;
	case QJsonValue::Array: {
		out.append('[');
		const auto arr = value.toArray();
		for (auto i = 0; i < arr.size(); ++i) {
			if (i > 0) {
				out.append(',');
			}
			AppendJsonValue(out, arr.at(i));
		}
		out.append(']');
		return;
	}
	case QJsonValue::Object: {
		out.append('{');
		const auto obj = value.toObject();
		auto keys = obj.keys();
		std::sort(keys.begin(), keys.end(), UnicodeCodePointLess);
		auto first = true;
		for (const auto &key : keys) {
			if (stripTopSignature && key == QStringLiteral("signature")) {
				continue;
			}
			if (!first) {
				out.append(',');
			}
			first = false;
			AppendJsonString(out, key);
			out.append(':');
			AppendJsonValue(out, obj.value(key));
		}
		out.append('}');
		return;
	}
	}
}

bool IsValidSha256Hex(const QString &value) {
	if (value.size() != 64) {
		return false;
	}
	for (const auto ch : value) {
		const auto u = ch.unicode();
		const auto isDigit = (u >= '0' && u <= '9');
		const auto isLowerHex = (u >= 'a' && u <= 'f');
		if (!isDigit && !isLowerHex) {
			return false;
		}
	}
	return true;
}

bool IsLocalHost(const QString &host) {
	const auto lower = host.toLower();
	return (lower == QStringLiteral("127.0.0.1"))
		|| (lower == QStringLiteral("localhost"))
		|| (lower == QStringLiteral("::1"));
}

std::optional<UpdateRelease> ParseReleaseEntry(
		const QJsonValue &value,
		UpdateChannel channel,
		const QUrl &manifestUrl,
		QString *error) {
	if (value.isNull() || value.isUndefined()) {
		return std::nullopt;
	}
	if (!value.isObject()) {
		SetError(error, QStringLiteral("Channel entry is not an object."));
		return std::nullopt;
	}
	const auto obj = value.toObject();
	auto release = UpdateRelease();
	release.sourceChannel = channel;
	release.build = static_cast<int>(obj.value(QStringLiteral("build")).toInteger(-1));
	release.ovygramVersion = obj.value(QStringLiteral("ovygram_version")).toString().trimmed();
	release.telegramVersion = obj.value(QStringLiteral("telegram_version")).toString().trimmed();
	release.telegramVersionNum = static_cast<int>(
		obj.value(QStringLiteral("telegram_version_num")).toInteger(-1));
	release.fileName = obj.value(QStringLiteral("file_name")).toString().trimmed();
	release.sha256 = obj.value(QStringLiteral("sha256")).toString().trimmed().toLower();
	release.size = obj.value(QStringLiteral("size")).toInteger(-1);
	release.url = obj.value(QStringLiteral("url")).toString().trimmed();
	release.commit = obj.value(QStringLiteral("commit")).toString().trimmed().toLower();
	release.notes = obj.value(QStringLiteral("notes")).toString();
	release.publishedAt = obj.value(QStringLiteral("published_at")).toString().trimmed();

	if (release.build <= 0
		|| release.ovygramVersion.isEmpty()
		|| release.telegramVersion.isEmpty()
		|| release.telegramVersionNum <= 0
		|| release.fileName.isEmpty()
		|| release.fileName.contains('/')
		|| release.fileName.contains('\\')
		|| !IsValidSha256Hex(release.sha256)
		|| release.size <= 0
		|| release.size > kMaxUpdatePackageSize) {
		SetError(error, QStringLiteral("Invalid release metadata fields."));
		return std::nullopt;
	}
	const auto url = QUrl(release.url);
	if (!IsAllowedDownloadUrl(url, manifestUrl)) {
		SetError(error, QStringLiteral("Disallowed update download URL."));
		return std::nullopt;
	}
	return release;
}

struct ParsedSemVer {
	std::vector<int> core;
	QStringList prerelease;
	bool valid = false;
};

ParsedSemVer ParseSemVer(const QString &raw) {
	auto result = ParsedSemVer();
	auto text = raw.trimmed();
	if (text.startsWith('v') || text.startsWith('V')) {
		text = text.mid(1);
	}
	const auto plusIdx = text.indexOf('+');
	if (plusIdx >= 0) {
		text = text.left(plusIdx);
	}
	auto coreText = text;
	const auto dashIdx = text.indexOf('-');
	if (dashIdx >= 0) {
		coreText = text.left(dashIdx);
		const auto pre = text.mid(dashIdx + 1);
		if (!pre.isEmpty()) {
			result.prerelease = pre.split('.');
		}
	}
	const auto parts = coreText.split('.');
	if (parts.isEmpty()) {
		return result;
	}
	for (const auto &part : parts) {
		auto ok = false;
		const auto num = part.toInt(&ok);
		if (!ok || num < 0) {
			return result;
		}
		result.core.push_back(num);
	}
	result.valid = true;
	return result;
}

} // namespace

QString UpdateChannelName(UpdateChannel channel) {
	switch (channel) {
	case UpdateChannel::Stable: return QStringLiteral("stable");
	case UpdateChannel::Beta: return QStringLiteral("beta");
	case UpdateChannel::Canary: return QStringLiteral("canary");
	}
	return QStringLiteral("stable");
}

std::optional<UpdateChannel> UpdateChannelFromName(const QString &name) {
	const auto lower = name.trimmed().toLower();
	if (lower == QStringLiteral("stable")) {
		return UpdateChannel::Stable;
	} else if (lower == QStringLiteral("beta")) {
		return UpdateChannel::Beta;
	} else if (lower == QStringLiteral("canary")) {
		return UpdateChannel::Canary;
	}
	return std::nullopt;
}

QString UpdateChannelTitle(UpdateChannel channel) {
	switch (channel) {
	case UpdateChannel::Stable: return QStringLiteral("Стабильный");
	case UpdateChannel::Beta: return QStringLiteral("Бета");
	case UpdateChannel::Canary: return QStringLiteral("Ночной (Canary)");
	}
	return QStringLiteral("Стабильный");
}

QString UpdateChannelDescription(UpdateChannel channel) {
	switch (channel) {
	case UpdateChannel::Stable:
		return QStringLiteral(
			"Проверенные релизы для повседневного использования.");
	case UpdateChannel::Beta:
		return QStringLiteral(
			"Предрелизные сборки с новыми функциями до выхода в стабильный канал.");
	case UpdateChannel::Canary:
		return QStringLiteral(
			"Свежие тестовые сборки из GitHub Actions. Возможны ошибки и нестабильность.");
	}
	return QString();
}

QString CurrentUpdatePlatformKey() {
#if defined Q_OS_WIN
#if defined Q_PROCESSOR_ARM_64
	return QStringLiteral("winarm64");
#elif QT_POINTER_SIZE == 8
	return QStringLiteral("win64");
#else
	return QStringLiteral("win32");
#endif
#elif defined Q_OS_MACOS
#if defined Q_PROCESSOR_ARM_64
	return QStringLiteral("macos-arm64");
#else
	return QStringLiteral("macos-x64");
#endif
#else
#if defined Q_PROCESSOR_ARM_64
	return QStringLiteral("linux-arm64");
#else
	return QStringLiteral("linux-x64");
#endif
#endif
}

std::vector<PinnedUpdateKey> DefaultPinnedUpdateKeys() {
	return {
		PinnedUpdateKey{
			.keyId = QString::fromLatin1(kPinnedUpdateKeyId),
			.rawPublicKey = QByteArray::fromBase64(kPinnedUpdatePublicKeyBase64),
		},
	};
}

QByteArray CanonicalManifestJson(const QJsonObject &object) {
	auto out = QByteArray();
	out.reserve(1024);
	AppendJsonValue(out, QJsonValue(object), true);
	return out;
}

bool VerifyEd25519Signature(
		const QByteArray &rawPublicKey,
		const QByteArray &message,
		const QByteArray &signature,
		QString *error) {
	if (rawPublicKey.size() != 32) {
		SetError(error, QStringLiteral("Invalid Ed25519 public key length."));
		return false;
	}
	if (signature.size() != 64) {
		SetError(error, QStringLiteral("Invalid Ed25519 signature length."));
		return false;
	}
	const auto key = std::unique_ptr<EVP_PKEY, PKeyDeleter>(
		EVP_PKEY_new_raw_public_key(
			EVP_PKEY_ED25519,
			nullptr,
			reinterpret_cast<const unsigned char*>(rawPublicKey.constData()),
			static_cast<size_t>(rawPublicKey.size())));
	if (!key) {
		SetError(error, QStringLiteral("Could not load Ed25519 public key."));
		return false;
	}
	const auto ctx = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>(EVP_MD_CTX_new());
	if (!ctx) {
		SetError(error, QStringLiteral("Could not allocate verification context."));
		return false;
	}
	if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
		SetError(error, QStringLiteral("EVP_DigestVerifyInit failed."));
		return false;
	}
	const auto ok = EVP_DigestVerify(
		ctx.get(),
		reinterpret_cast<const unsigned char*>(signature.constData()),
		static_cast<size_t>(signature.size()),
		reinterpret_cast<const unsigned char*>(message.constData()),
		static_cast<size_t>(message.size()));
	if (ok != 1) {
		SetError(error, QStringLiteral("Ed25519 signature verification failed."));
		return false;
	}
	return true;
}

bool IsAllowedDownloadUrl(const QUrl &url, const QUrl &manifestUrl) {
	if (!url.isValid() || url.host().isEmpty()) {
		return false;
	}
	const auto scheme = url.scheme().toLower();
	const auto host = url.host().toLower();
	const auto manifestHost = manifestUrl.host().toLower();
	if (scheme == QStringLiteral("http")) {
		return manifestUrl.scheme().toLower() == QStringLiteral("http")
			&& IsLocalHost(manifestHost)
			&& host == manifestHost;
	}
	if (scheme != QStringLiteral("https")) {
		return false;
	}
	return (host == QStringLiteral("github.com"))
		|| (host == QStringLiteral("objects.githubusercontent.com"))
		|| (host == QStringLiteral("release-assets.githubusercontent.com"))
		|| (host == QStringLiteral("ovygram.opik.net"))
		|| (host == QStringLiteral("api.ovygram.opik.net"))
		|| (!manifestHost.isEmpty() && host == manifestHost);
}

std::optional<VerifiedUpdateManifest> ParseAndVerifyManifest(
		const QByteArray &jsonBytes,
		const QString &platform,
		const QUrl &manifestUrl,
		const QDateTime &nowUtc,
		const std::vector<PinnedUpdateKey> &keys,
		QString *error) {
	if (jsonBytes.isEmpty() || jsonBytes.size() > kMaxUpdateManifestSize) {
		SetError(error, QStringLiteral("Manifest response size is out of bounds."));
		return std::nullopt;
	}
	auto parseError = QJsonParseError();
	const auto document = QJsonDocument::fromJson(jsonBytes, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		SetError(error, QStringLiteral("Manifest is not valid JSON."));
		return std::nullopt;
	}
	const auto root = document.object();
	const auto keyId = root.value(QStringLiteral("key_id")).toString().trimmed();
	const auto signatureB64 = root.value(QStringLiteral("signature")).toString().trimmed();
	if (keyId.isEmpty() || signatureB64.isEmpty()) {
		SetError(error, QStringLiteral("Manifest is missing key_id or signature."));
		return std::nullopt;
	}

	const auto keyIt = std::find_if(keys.begin(), keys.end(), [&](const PinnedUpdateKey &item) {
		return item.keyId == keyId;
	});
	if (keyIt == keys.end()) {
		SetError(error, QStringLiteral("Manifest signed by unknown key_id."));
		return std::nullopt;
	}

	const auto signature = QByteArray::fromBase64(signatureB64.toLatin1());
	const auto canonical = CanonicalManifestJson(root);
	if (!VerifyEd25519Signature(keyIt->rawPublicKey, canonical, signature, error)) {
		return std::nullopt;
	}

	const auto schema = static_cast<int>(root.value(QStringLiteral("schema")).toInteger(-1));
	if (schema != 1) {
		SetError(error, QStringLiteral("Unsupported update manifest schema version."));
		return std::nullopt;
	}

	const auto issuedAt = QDateTime::fromString(
		root.value(QStringLiteral("issued_at")).toString(),
		Qt::ISODate).toUTC();
	const auto validUntil = QDateTime::fromString(
		root.value(QStringLiteral("valid_until")).toString(),
		Qt::ISODate).toUTC();
	if (!issuedAt.isValid() || !validUntil.isValid() || validUntil <= issuedAt) {
		SetError(error, QStringLiteral("Manifest timestamps are invalid."));
		return std::nullopt;
	}
	if (nowUtc.isValid()) {
		if (nowUtc > validUntil.addSecs(kClockSkewAllowanceSeconds)) {
			SetError(error, QStringLiteral("Update manifest has expired."));
			return std::nullopt;
		}
		if (issuedAt > nowUtc.addSecs(kClockSkewAllowanceSeconds)) {
			SetError(error, QStringLiteral("Update manifest issued_at is in the future."));
			return std::nullopt;
		}
	}

	const auto platformsVal = root.value(QStringLiteral("platforms"));
	if (!platformsVal.isObject()) {
		SetError(error, QStringLiteral("Manifest is missing platforms object."));
		return std::nullopt;
	}

	auto verified = VerifiedUpdateManifest();
	verified.schema = schema;
	verified.issuedAt = issuedAt;
	verified.validUntil = validUntil;
	verified.keyId = keyId;
	verified.platform = platform;

	const auto platformVal = platformsVal.toObject().value(platform);
	if (platformVal.isUndefined() || platformVal.isNull()) {
		return verified;
	}
	if (!platformVal.isObject()) {
		SetError(error, QStringLiteral("Manifest platform entry is not an object."));
		return std::nullopt;
	}
	const auto channels = platformVal.toObject();
	const auto parseOne = [&](const QString &name, UpdateChannel ch, std::optional<UpdateRelease> &slot) {
		const auto val = channels.value(name);
		if (val.isUndefined() || val.isNull()) {
			return true;
		}
		auto parsed = ParseReleaseEntry(val, ch, manifestUrl, error);
		if (!parsed) {
			return false;
		}
		slot = std::move(parsed);
		return true;
	};
	if (!parseOne(QStringLiteral("stable"), UpdateChannel::Stable, verified.stable)
		|| !parseOne(QStringLiteral("beta"), UpdateChannel::Beta, verified.beta)
		|| !parseOne(QStringLiteral("canary"), UpdateChannel::Canary, verified.canary)) {
		return std::nullopt;
	}
	return verified;
}

int CompareSemVer(const QString &left, const QString &right) {
	const auto a = ParseSemVer(left);
	const auto b = ParseSemVer(right);
	if (!a.valid || !b.valid) {
		return left.compare(right);
	}
	const auto maxCore = std::max(a.core.size(), b.core.size());
	for (size_t i = 0; i < maxCore; ++i) {
		const auto av = (i < a.core.size()) ? a.core[i] : 0;
		const auto bv = (i < b.core.size()) ? b.core[i] : 0;
		if (av != bv) {
			return (av < bv) ? -1 : 1;
		}
	}
	const auto aPre = !a.prerelease.isEmpty();
	const auto bPre = !b.prerelease.isEmpty();
	if (aPre != bPre) {
		// Release without prerelease suffix is newer than prerelease.
		return aPre ? -1 : 1;
	}
	const auto maxPre = std::max(a.prerelease.size(), b.prerelease.size());
	for (qsizetype i = 0; i < maxPre; ++i) {
		if (i >= a.prerelease.size()) {
			return -1;
		}
		if (i >= b.prerelease.size()) {
			return 1;
		}
		auto aOk = false;
		auto bOk = false;
		const auto aNum = a.prerelease[i].toInt(&aOk);
		const auto bNum = b.prerelease[i].toInt(&bOk);
		if (aOk && bOk) {
			if (aNum != bNum) {
				return (aNum < bNum) ? -1 : 1;
			}
		} else if (aOk != bOk) {
			return aOk ? -1 : 1;
		} else {
			const auto cmp = a.prerelease[i].compare(b.prerelease[i]);
			if (cmp != 0) {
				return (cmp < 0) ? -1 : 1;
			}
		}
	}
	return 0;
}

int CompareReleases(const UpdateRelease &left, const UpdateRelease &right) {
	if (const auto ver = CompareSemVer(left.ovygramVersion, right.ovygramVersion); ver != 0) {
		return ver;
	}
	if (left.telegramVersionNum != right.telegramVersionNum) {
		return (left.telegramVersionNum < right.telegramVersionNum) ? -1 : 1;
	}
	if (left.build != right.build) {
		return (left.build < right.build) ? -1 : 1;
	}
	// Prefer more stable channel on exact tie.
	if (left.sourceChannel != right.sourceChannel) {
		return (static_cast<int>(left.sourceChannel) > static_cast<int>(right.sourceChannel))
			? -1
			: 1;
	}
	return 0;
}

bool IsReleaseNewer(
		const UpdateRelease &candidate,
		const InstalledVersion &current) {
	// Never re-download the exact same commit we are already running.
	if (!candidate.commit.isEmpty()
		&& !current.commit.isEmpty()
		&& candidate.commit.compare(current.commit, Qt::CaseInsensitive) == 0) {
		return false;
	}
	// Rollback protection: when build metadata is present, require strictly
	// higher monotonic build number unless version itself jumped.
	const auto verCmp = CompareSemVer(candidate.ovygramVersion, current.ovygramVersion);
	if (verCmp != 0) {
		return verCmp > 0;
	}
	if (candidate.telegramVersionNum != current.telegramVersionNum) {
		return candidate.telegramVersionNum > current.telegramVersionNum;
	}
	if (current.build > 0) {
		return candidate.build > current.build;
	}
	// Fresh manual install without ovygram_build.json: update if commit differs
	// (when compiled with OVYGRAM_BUILD_COMMIT) or if candidate has a newer build.
	if (!current.commit.isEmpty() && !candidate.commit.isEmpty()) {
		return true;
	}
	return false;
}

std::optional<UpdateRelease> SelectUpdateForChannel(
		const VerifiedUpdateManifest &manifest,
		UpdateChannel selectedChannel,
		const InstalledVersion &current) {
	auto candidates = std::vector<UpdateRelease>();
	if (manifest.stable) {
		candidates.push_back(*manifest.stable);
	}
	if (selectedChannel == UpdateChannel::Beta || selectedChannel == UpdateChannel::Canary) {
		if (manifest.beta) {
			candidates.push_back(*manifest.beta);
		}
	}
	if (selectedChannel == UpdateChannel::Canary) {
		if (manifest.canary) {
			candidates.push_back(*manifest.canary);
		}
	}
	auto best = std::optional<UpdateRelease>();
	for (const auto &item : candidates) {
		if (!IsReleaseNewer(item, current)) {
			continue;
		}
		if (!best || CompareReleases(*best, item) < 0) {
			best = item;
		}
	}
	return best;
}

} // namespace Ovygram
