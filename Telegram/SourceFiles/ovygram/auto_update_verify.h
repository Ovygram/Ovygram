#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QUrl>

#include <optional>
#include <vector>

namespace Ovygram {

inline constexpr auto kDefaultUpdateManifestUrl
	= "https://api.ovygram.opik.net/v1/updates/manifest";
inline constexpr auto kPinnedUpdateKeyId = "8e957eed7ec45741";
inline constexpr auto kPinnedUpdatePublicKeyBase64
	= "B1jUwe/ZTFDk6apqN0Yke0CKfl8t9tSm1yt88g1nljA=";
inline constexpr auto kMaxUpdatePackageSize = qint64(256) * 1024 * 1024;
inline constexpr auto kMaxUpdateManifestSize = qint64(512) * 1024;

enum class UpdateChannel : int {
	Stable = 0,
	Beta = 1,
	Canary = 2,
};

struct PinnedUpdateKey {
	QString keyId;
	QByteArray rawPublicKey; // 32 raw Ed25519 bytes.
};

struct UpdateRelease {
	int build = 0;
	QString ovygramVersion;
	QString telegramVersion;
	int telegramVersionNum = 0;
	QString fileName;
	QString sha256;
	qint64 size = 0;
	QString url;
	QString commit;
	QString notes;
	QString publishedAt;
	UpdateChannel sourceChannel = UpdateChannel::Stable;
};

struct InstalledVersion {
	QString ovygramVersion;
	int telegramVersionNum = 0;
	int build = 0;
	QString commit;
};

struct VerifiedUpdateManifest {
	int schema = 0;
	QDateTime issuedAt;
	QDateTime validUntil;
	QString keyId;
	QString platform;
	std::optional<UpdateRelease> stable;
	std::optional<UpdateRelease> beta;
	std::optional<UpdateRelease> canary;
};

[[nodiscard]] QString UpdateChannelName(UpdateChannel channel);
[[nodiscard]] std::optional<UpdateChannel> UpdateChannelFromName(const QString &name);
[[nodiscard]] QString UpdateChannelTitle(UpdateChannel channel);
[[nodiscard]] QString UpdateChannelDescription(UpdateChannel channel);
[[nodiscard]] QString CurrentUpdatePlatformKey();

[[nodiscard]] std::vector<PinnedUpdateKey> DefaultPinnedUpdateKeys();

// Serializes object into Python-compatible canonical JSON UTF-8 bytes
// (sort_keys=True, separators=(",", ":"), ensure_ascii=False), excluding
// top-level "signature".
[[nodiscard]] QByteArray CanonicalManifestJson(const QJsonObject &object);

[[nodiscard]] bool VerifyEd25519Signature(
	const QByteArray &rawPublicKey,
	const QByteArray &message,
	const QByteArray &signature,
	QString *error = nullptr);

[[nodiscard]] std::optional<VerifiedUpdateManifest> ParseAndVerifyManifest(
	const QByteArray &jsonBytes,
	const QString &platform,
	const QUrl &manifestUrl,
	const QDateTime &nowUtc = QDateTime::currentDateTimeUtc(),
	const std::vector<PinnedUpdateKey> &keys = DefaultPinnedUpdateKeys(),
	QString *error = nullptr);

[[nodiscard]] int CompareSemVer(const QString &left, const QString &right);
[[nodiscard]] int CompareReleases(const UpdateRelease &left, const UpdateRelease &right);
[[nodiscard]] bool IsReleaseNewer(
	const UpdateRelease &candidate,
	const InstalledVersion &current);
[[nodiscard]] std::optional<UpdateRelease> SelectUpdateForChannel(
	const VerifiedUpdateManifest &manifest,
	UpdateChannel selectedChannel,
	const InstalledVersion &current);
[[nodiscard]] bool IsAllowedDownloadUrl(const QUrl &url, const QUrl &manifestUrl);

} // namespace Ovygram
