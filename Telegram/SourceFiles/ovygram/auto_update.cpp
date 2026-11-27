#include "ovygram/auto_update.h"

#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/version.h"
#include "logs.h"
#include "settings.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkRequest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif // Q_OS_WIN

namespace Ovygram {
namespace {

constexpr auto kNormalCheckDelaySeconds = 4 * 3600;
constexpr auto kCanaryCheckDelaySeconds = 3600;
constexpr auto kRetryDelaySeconds = 600;
constexpr auto kStartupDelayMs = crl::time(10'000);
constexpr auto kMaxRedirects = 5;

#ifdef OVYGRAM_BUILD_COMMIT
inline constexpr auto kCompiledBuildCommit = QT_STRINGIFY(OVYGRAM_BUILD_COMMIT);
#else
inline constexpr auto kCompiledBuildCommit = "";
#endif

QString UpdatesDirPath() {
	return cWorkingDir() + u"tupdates"_q;
}

QString ChannelFilePath() {
	return cWorkingDir() + u"tdata/ovygram_update_channel"_q;
}

QString ReadyZipPath() {
	return UpdatesDirPath() + u"/ovygram_ready.zip"_q;
}

QString ReadyMetaPath() {
	return UpdatesDirPath() + u"/ovygram_ready.json"_q;
}

QString DownloadPartPath() {
	return UpdatesDirPath() + u"/ovygram_download.part"_q;
}

QString InstalledBuildMetaPath() {
	return cExeDir() + u"ovygram_build.json"_q;
}

QUrl ResolveManifestUrl() {
	const auto envUrl = qEnvironmentVariable("OVYGRAM_UPDATE_MANIFEST_URL").trimmed();
	if (!envUrl.isEmpty()) {
		const auto parsed = QUrl(envUrl);
		if (parsed.isValid() && !parsed.host().isEmpty()) {
			return parsed;
		}
	}
	return QUrl(QString::fromLatin1(kDefaultUpdateManifestUrl));
}

QJsonObject ReleaseToJson(const UpdateRelease &release) {
	return QJsonObject{
		{ u"build"_q, release.build },
		{ u"ovygram_version"_q, release.ovygramVersion },
		{ u"telegram_version"_q, release.telegramVersion },
		{ u"telegram_version_num"_q, release.telegramVersionNum },
		{ u"file_name"_q, release.fileName },
		{ u"sha256"_q, release.sha256 },
		{ u"size"_q, release.size },
		{ u"url"_q, release.url },
		{ u"commit"_q, release.commit },
		{ u"notes"_q, release.notes },
		{ u"published_at"_q, release.publishedAt },
		{ u"channel"_q, UpdateChannelName(release.sourceChannel) },
	};
}

std::optional<UpdateRelease> ReleaseFromJson(const QJsonObject &obj) {
	auto release = UpdateRelease();
	release.build = static_cast<int>(obj.value(u"build"_q).toInteger(-1));
	release.ovygramVersion = obj.value(u"ovygram_version"_q).toString().trimmed();
	release.telegramVersion = obj.value(u"telegram_version"_q).toString().trimmed();
	release.telegramVersionNum = static_cast<int>(
		obj.value(u"telegram_version_num"_q).toInteger(-1));
	release.fileName = obj.value(u"file_name"_q).toString().trimmed();
	release.sha256 = obj.value(u"sha256"_q).toString().trimmed().toLower();
	release.size = obj.value(u"size"_q).toInteger(-1);
	release.url = obj.value(u"url"_q).toString().trimmed();
	release.commit = obj.value(u"commit"_q).toString().trimmed().toLower();
	release.notes = obj.value(u"notes"_q).toString();
	release.publishedAt = obj.value(u"published_at"_q).toString().trimmed();
	if (const auto ch = UpdateChannelFromName(obj.value(u"channel"_q).toString())) {
		release.sourceChannel = *ch;
	}
	if (release.build <= 0
		|| release.ovygramVersion.isEmpty()
		|| release.telegramVersion.isEmpty()
		|| release.telegramVersionNum <= 0
		|| release.sha256.size() != 64
		|| release.size <= 0
		|| release.size > kMaxUpdatePackageSize) {
		return std::nullopt;
	}
	return release;
}

QString ComputeFileSha256(const QString &path, qint64 *outSize = nullptr) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return QString();
	}
	QCryptographicHash hash(QCryptographicHash::Sha256);
	qint64 total = 0;
	QByteArray buffer;
	buffer.resize(65536);
	while (true) {
		const auto read = file.read(buffer.data(), buffer.size());
		if (read < 0) {
			return QString();
		}
		if (read == 0) {
			break;
		}
		total += read;
		if (total > kMaxUpdatePackageSize) {
			return QString();
		}
		hash.addData(QByteArrayView(buffer.constData(), read));
	}
	if (outSize) {
		*outSize = total;
	}
	return QString::fromLatin1(hash.result().toHex()).toLower();
}

InstalledVersion ReadInstalledVersion() {
	auto current = InstalledVersion();
	current.ovygramVersion = QString::fromLatin1(OvygramVersionStr);
	current.telegramVersionNum = AppVersion;
	current.commit = QString::fromLatin1(kCompiledBuildCommit).trimmed().toLower();

	QFile metaFile(InstalledBuildMetaPath());
	if (!metaFile.open(QIODevice::ReadOnly)) {
		return current;
	}
	const auto doc = QJsonDocument::fromJson(metaFile.read(65536));
	if (!doc.isObject()) {
		return current;
	}
	const auto obj = doc.object();
	const auto metaOvy = obj.value(u"ovygram_version"_q).toString().trimmed();
	const auto metaTg = static_cast<int>(obj.value(u"telegram_version_num"_q).toInteger(0));
	// Trust ovygram_build.json only when it matches the running binary's compiled versions.
	if (metaOvy == current.ovygramVersion && metaTg == current.telegramVersionNum) {
		const auto build = static_cast<int>(obj.value(u"build"_q).toInteger(0));
		if (build > 0) {
			current.build = build;
		}
		const auto commit = obj.value(u"commit"_q).toString().trimmed().toLower();
		if (!commit.isEmpty()) {
			current.commit = commit;
		}
	}
	return current;
}

std::optional<UpdateRelease> VerifyStagedUpdate() {
	QFile metaFile(ReadyMetaPath());
	if (!metaFile.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	const auto doc = QJsonDocument::fromJson(metaFile.read(128 * 1024));
	if (!doc.isObject()) {
		return std::nullopt;
	}
	const auto release = ReleaseFromJson(doc.object());
	if (!release) {
		return std::nullopt;
	}
	if (!IsReleaseNewer(*release, ReadInstalledVersion())) {
		return std::nullopt;
	}
	qint64 actualSize = 0;
	const auto actualSha = ComputeFileSha256(ReadyZipPath(), &actualSize);
	if (actualSha.isEmpty() || actualSize != release->size || actualSha != release->sha256) {
		return std::nullopt;
	}
	return release;
}

#ifdef Q_OS_WIN
QString QuoteWinArg(const QString &arg) {
	if (arg.isEmpty()) {
		return u"\"\""_q;
	}
	auto needsQuotes = false;
	for (const auto ch : arg) {
		if (ch == ' ' || ch == '\t' || ch == '"') {
			needsQuotes = true;
			break;
		}
	}
	if (!needsQuotes) {
		return arg;
	}
	auto result = QString();
	result.reserve(arg.size() + 4);
	result.append('"');
	auto backslashes = 0;
	for (const auto ch : arg) {
		if (ch == '\\') {
			++backslashes;
		} else if (ch == '"') {
			result.append(QString(backslashes * 2 + 1, '\\'));
			result.append('"');
			backslashes = 0;
		} else {
			if (backslashes > 0) {
				result.append(QString(backslashes, '\\'));
				backslashes = 0;
			}
			result.append(ch);
		}
	}
	if (backslashes > 0) {
		result.append(QString(backslashes * 2, '\\'));
	}
	result.append('"');
	return result;
}
#endif // Q_OS_WIN

} // namespace

AutoUpdate &AutoUpdate::Instance() {
	static auto instance = AutoUpdate();
	return instance;
}

AutoUpdate::AutoUpdate()
: _timer([this] { start(false); }) {
	loadChannelSetting();
}

AutoUpdate::~AutoUpdate() {
	stop();
}

rpl::producer<> AutoUpdate::checking() const {
	return _checking.events();
}

rpl::producer<> AutoUpdate::isLatest() const {
	return _isLatest.events();
}

rpl::producer<AutoUpdate::Progress> AutoUpdate::progress() const {
	return _progress.events();
}

rpl::producer<> AutoUpdate::failed() const {
	return _failed.events();
}

rpl::producer<> AutoUpdate::ready() const {
	return _ready.events();
}

AutoUpdate::State AutoUpdate::state() const {
	return _state;
}

int AutoUpdate::already() const {
	return _already;
}

int AutoUpdate::size() const {
	return _size;
}

bool AutoUpdate::percent() const {
	return false;
}

UpdateChannel AutoUpdate::channel() const {
	return _channel;
}

rpl::producer<UpdateChannel> AutoUpdate::channelChanged() const {
	return _channelChanged.events();
}

std::optional<UpdateRelease> AutoUpdate::readyRelease() const {
	return _readyRelease;
}

InstalledVersion AutoUpdate::installedVersion() const {
	return ReadInstalledVersion();
}

void AutoUpdate::loadChannelSetting() {
	if (_channelLoaded || cWorkingDir().isEmpty()) {
		return;
	}
	_channelLoaded = true;
	QFile file(ChannelFilePath());
	if (file.open(QIODevice::ReadOnly)) {
		const auto raw = QString::fromUtf8(file.read(64)).trimmed();
		if (const auto parsed = UpdateChannelFromName(raw)) {
			_channel = *parsed;
		}
	}
}

void AutoUpdate::saveChannelSetting(UpdateChannel channel) const {
	if (cWorkingDir().isEmpty()) {
		return;
	}
	QDir().mkpath(cWorkingDir() + u"tdata"_q);
	QSaveFile file(ChannelFilePath());
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(UpdateChannelName(channel).toUtf8());
		file.commit();
	}
}

void AutoUpdate::setChannel(UpdateChannel channel) {
	loadChannelSetting();
	if (_channel == channel) {
		return;
	}
	_channel = channel;
	saveChannelSetting(channel);
	_channelChanged.fire_copy(channel);

	stop();
	clearStagedFiles();
	_readyRelease.reset();
	_state = State::None;
	cSetLastUpdateCheck(0);
	if (cAutoUpdate()) {
		start(false);
	}
}

void AutoUpdate::clearActiveRequests() {
	if (_manifestReply) {
		auto reply = _manifestReply;
		_manifestReply = nullptr;
		reply->disconnect();
		reply->abort();
		reply->deleteLater();
	}
	if (_downloadReply) {
		auto reply = _downloadReply;
		_downloadReply = nullptr;
		reply->disconnect();
		reply->abort();
		reply->deleteLater();
	}
	if (_downloadFile) {
		_downloadFile->close();
		_downloadFile.reset();
	}
	_downloadHash.reset();
	_downloadingRelease.reset();
	_downloadBytes = 0;
}

void AutoUpdate::clearStagedFiles() {
	QFile::remove(DownloadPartPath());
	QFile::remove(ReadyZipPath());
	QFile::remove(ReadyMetaPath());
}

bool AutoUpdate::loadExistingReadyUpdate() {
	if (const auto staged = VerifyStagedUpdate()) {
		_readyRelease = *staged;
		_state = State::Ready;
		return true;
	}
	return false;
}

void AutoUpdate::stop() {
	_timer.cancel();
	clearActiveRequests();
	if (_state == State::Download) {
		_state = State::None;
		_already = 0;
		_size = 0;
	}
}

void AutoUpdate::scheduleNextCheck(bool forceWait) {
	if (!cAutoUpdate()) {
		return;
	}
	const auto interval = (_channel == UpdateChannel::Canary)
		? kCanaryCheckDelaySeconds
		: kNormalCheckDelaySeconds;
	const auto now = base::unixtime::now();
	const auto last = cLastUpdateCheck();
	auto delayMs = crl::time(0);
	if (forceWait) {
		delayMs = kStartupDelayMs;
	} else if (last > 0 && last <= now && (now - last) < interval) {
		delayMs = crl::time(interval - (now - last)) * 1000;
	}
	if (delayMs > 0) {
		_timer.callOnce(delayMs);
	} else {
		requestManifest();
	}
}

void AutoUpdate::start(bool forceWait) {
	loadChannelSetting();
	if (Core::UpdaterDisabled()) {
		return;
	}
	if (_manifestReply || _downloadReply) {
		return;
	}
	if (!cAutoUpdate() && forceWait) {
		return;
	}
	if (_state == State::Ready || loadExistingReadyUpdate()) {
		_ready.fire({});
		return;
	}
	if (forceWait || cLastUpdateCheck() > 0) {
		scheduleNextCheck(forceWait);
		return;
	}
	requestManifest();
}

void AutoUpdate::requestManifest() {
	clearActiveRequests();
	if (!_network) {
		_network = std::make_unique<QNetworkAccessManager>();
	}
	auto manifestUrl = ResolveManifestUrl();
	auto query = QUrlQuery(manifestUrl);
	const auto platform = CurrentUpdatePlatformKey();
	if (!query.hasQueryItem(u"platform"_q)) {
		query.addQueryItem(u"platform"_q, platform);
		manifestUrl.setQuery(query);
	}
	LOG(("Ovygram Update: checking manifest at '%1' (channel=%2).")
		.arg(manifestUrl.toString(), UpdateChannelName(_channel)));

	_checking.fire({});

	auto request = QNetworkRequest(manifestUrl);
	request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setHeader(
		QNetworkRequest::UserAgentHeader,
		u"Ovygram/%1-%2 (%3)"_q.arg(
			QString::fromLatin1(OvygramVersionStr),
			QString::fromLatin1(AppVersionStr),
			platform));

	QPointer<QNetworkReply> reply = _network->get(request);
	_manifestReply = reply;
	reply->setReadBufferSize(kMaxUpdateManifestSize);
	QObject::connect(reply, &QNetworkReply::finished, [this, reply, manifestUrl] {
		handleManifestReply(reply, manifestUrl);
	});
}

void AutoUpdate::handleManifestReply(
		QPointer<QNetworkReply> reply,
		const QUrl &manifestUrl) {
	if (!reply || reply != _manifestReply) {
		if (reply) {
			reply->deleteLater();
		}
		return;
	}
	_manifestReply = nullptr;
	const auto netError = reply->error();
	const auto httpStatus = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const auto body = reply->read(kMaxUpdateManifestSize + 1);
	reply->deleteLater();

	cSetLastUpdateCheck(base::unixtime::now());
	if (netError != QNetworkReply::NoError || httpStatus != 200) {
		LOG(("Ovygram Update Error: manifest request failed (http=%1, err=%2).")
			.arg(httpStatus)
			.arg(int(netError)));
		failCheck();
		return;
	}

	auto error = QString();
	const auto platform = CurrentUpdatePlatformKey();
	const auto verified = ParseAndVerifyManifest(
		body,
		platform,
		manifestUrl,
		QDateTime::currentDateTimeUtc(),
		DefaultPinnedUpdateKeys(),
		&error);
	if (!verified) {
		LOG(("Ovygram Update Error: manifest verification failed: %1").arg(error));
		failCheck();
		return;
	}

	const auto selected = SelectUpdateForChannel(
		*verified,
		_channel,
		ReadInstalledVersion());
	if (!selected) {
		LOG(("Ovygram Update: client is up to date on channel '%1'.")
			.arg(UpdateChannelName(_channel)));
		_state = State::None;
		_isLatest.fire({});
		scheduleNextCheck(false);
		return;
	}

	if (const auto staged = VerifyStagedUpdate();
		staged && staged->sha256 == selected->sha256) {
		_readyRelease = *staged;
		_state = State::Ready;
		_ready.fire({});
		return;
	}

	startDownload(*selected, manifestUrl);
}

void AutoUpdate::startDownload(
		const UpdateRelease &release,
		const QUrl &manifestUrl) {
	clearActiveRequests();
	QDir().mkpath(UpdatesDirPath());
	QFile::remove(DownloadPartPath());

	_downloadFile = std::make_unique<QFile>(DownloadPartPath());
	if (!_downloadFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		LOG(("Ovygram Update Error: could not open '%1' for writing.")
			.arg(DownloadPartPath()));
		failCheck();
		return;
	}
	_downloadHash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
	_downloadingRelease = release;
	_downloadManifestUrl = manifestUrl;
	_downloadBytes = 0;
	_state = State::Download;
	_already = 0;
	_size = static_cast<int>(std::min<qint64>(release.size, INT_MAX));
	_progress.fire({ _already, _size, false });

	LOG(("Ovygram Update: downloading %1-%2 build #%3 (%4 bytes).")
		.arg(release.ovygramVersion, release.telegramVersion)
		.arg(release.build)
		.arg(release.size));
	startDownloadRequest(QUrl(release.url), kMaxRedirects);
}

void AutoUpdate::startDownloadRequest(const QUrl &url, int redirectsLeft) {
	if (!IsAllowedDownloadUrl(url, _downloadManifestUrl)) {
		LOG(("Ovygram Update Error: blocked download URL '%1'.").arg(url.toString()));
		failCheck();
		return;
	}
	if (!_network) {
		_network = std::make_unique<QNetworkAccessManager>();
	}
	_downloadRedirectsLeft = redirectsLeft;

	auto request = QNetworkRequest(url);
	request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::ManualRedirectPolicy);
	request.setHeader(
		QNetworkRequest::UserAgentHeader,
		u"Ovygram/%1-%2"_q.arg(
			QString::fromLatin1(OvygramVersionStr),
			QString::fromLatin1(AppVersionStr)));

	QPointer<QNetworkReply> reply = _network->get(request);
	_downloadReply = reply;
	QObject::connect(reply, &QNetworkReply::readyRead, [this, reply] {
		handleDownloadReadyRead(reply);
	});
	QObject::connect(reply, &QNetworkReply::finished, [this, reply] {
		handleDownloadFinished(reply);
	});
}

void AutoUpdate::handleDownloadReadyRead(QPointer<QNetworkReply> reply) {
	if (!reply || reply != _downloadReply || !_downloadFile || !_downloadHash || !_downloadingRelease) {
		return;
	}
	const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (status >= 300 && status < 400) {
		return;
	}
	const auto chunk = reply->readAll();
	if (chunk.isEmpty()) {
		return;
	}
	_downloadBytes += chunk.size();
	if (_downloadBytes > _downloadingRelease->size || _downloadBytes > kMaxUpdatePackageSize) {
		LOG(("Ovygram Update Error: downloaded stream exceeded expected size."));
		failCheck();
		return;
	}
	if (_downloadFile->write(chunk) != chunk.size()) {
		LOG(("Ovygram Update Error: disk write failed during update download."));
		failCheck();
		return;
	}
	_downloadHash->addData(chunk);
	_already = static_cast<int>(std::min<qint64>(_downloadBytes, INT_MAX));
	_progress.fire({ _already, _size, false });
}

void AutoUpdate::handleDownloadFinished(QPointer<QNetworkReply> reply) {
	if (!reply || reply != _downloadReply) {
		if (reply) {
			reply->deleteLater();
		}
		return;
	}
	_downloadReply = nullptr;
	const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const auto redirectTarget = reply->attribute(
		QNetworkRequest::RedirectionTargetAttribute).toUrl();
	const auto originUrl = reply->url();
	const auto netError = reply->error();

	if (status >= 300 && status < 400 && redirectTarget.isValid()) {
		reply->deleteLater();
		if (_downloadRedirectsLeft <= 0) {
			LOG(("Ovygram Update Error: too many redirects while downloading package."));
			failCheck();
			return;
		}
		const auto nextUrl = originUrl.resolved(redirectTarget);
		startDownloadRequest(nextUrl, _downloadRedirectsLeft - 1);
		return;
	}

	// Drain any final buffered bytes.
	if (netError == QNetworkReply::NoError && status == 200 && _downloadFile && _downloadHash && _downloadingRelease) {
		const auto tail = reply->readAll();
		if (!tail.isEmpty()) {
			_downloadBytes += tail.size();
			if (_downloadBytes <= _downloadingRelease->size
				&& _downloadFile->write(tail) == tail.size()) {
				_downloadHash->addData(tail);
			} else {
				reply->deleteLater();
				failCheck();
				return;
			}
		}
	}
	reply->deleteLater();

	if (netError != QNetworkReply::NoError || status != 200 || !_downloadingRelease || !_downloadFile || !_downloadHash) {
		LOG(("Ovygram Update Error: package download failed (http=%1, err=%2).")
			.arg(status)
			.arg(int(netError)));
		failCheck();
		return;
	}

	const auto release = *_downloadingRelease;
	const auto actualSha = QString::fromLatin1(_downloadHash->result().toHex()).toLower();
	const auto actualBytes = _downloadBytes;
	_downloadFile->flush();
	_downloadFile->close();
	_downloadFile.reset();
	_downloadHash.reset();
	_downloadingRelease.reset();

	if (actualBytes != release.size || actualSha != release.sha256) {
		LOG(("Ovygram Update Error: downloaded package checksum/size mismatch (expected %1, got %2).")
			.arg(release.sha256, actualSha));
		QFile::remove(DownloadPartPath());
		failCheck();
		return;
	}

	QFile::remove(ReadyZipPath());
	if (!QFile::rename(DownloadPartPath(), ReadyZipPath())) {
		LOG(("Ovygram Update Error: could not finalize staged package."));
		QFile::remove(DownloadPartPath());
		failCheck();
		return;
	}

	QSaveFile metaFile(ReadyMetaPath());
	if (!metaFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
		|| metaFile.write(QJsonDocument(ReleaseToJson(release)).toJson(QJsonDocument::Indented)) < 0
		|| !metaFile.commit()) {
		LOG(("Ovygram Update Error: could not write staged metadata."));
		clearStagedFiles();
		failCheck();
		return;
	}

	_readyRelease = release;
	_state = State::Ready;
	_already = _size;
	LOG(("Ovygram Update: package %1-%2 build #%3 verified and ready.")
		.arg(release.ovygramVersion, release.telegramVersion)
		.arg(release.build));
	_ready.fire({});
}

void AutoUpdate::failCheck() {
	clearActiveRequests();
	QFile::remove(DownloadPartPath());
	_state = State::None;
	_already = 0;
	_size = 0;
	_failed.fire({});
	if (cAutoUpdate()) {
		_timer.callOnce(crl::time(kRetryDelaySeconds) * 1000);
	}
}

bool AutoUpdate::CheckReadyUpdate() {
	if (VerifyStagedUpdate().has_value()) {
		return true;
	}
	QFile::remove(DownloadPartPath());
	QFile::remove(ReadyZipPath());
	QFile::remove(ReadyMetaPath());
	return false;
}

bool AutoUpdate::LaunchUpdater(
		bool customWorkingDir,
		const QString &workingDir,
		const QString &dataFile) {
#ifdef Q_OS_WIN
	const auto staged = VerifyStagedUpdate();
	if (!staged) {
		LOG(("Ovygram Update Error: staged update failed verification before launch."));
		return false;
	}
	const auto sourceUpdater = cExeDir() + u"OvygramUpdater.exe"_q;
	if (!QFileInfo::exists(sourceUpdater)) {
		LOG(("Ovygram Update Error: OvygramUpdater.exe not found at '%1'.").arg(sourceUpdater));
		return false;
	}
	const auto runningUpdater = UpdatesDirPath() + u"/OvygramUpdater.exe"_q;
	QFile::remove(runningUpdater);
	if (!QFile::copy(sourceUpdater, runningUpdater)) {
		LOG(("Ovygram Update Error: could not stage OvygramUpdater.exe to '%1'.").arg(runningUpdater));
		return false;
	}

	auto args = QStringList();
	args << u"--package"_q << QDir::toNativeSeparators(ReadyZipPath());
	args << u"--meta"_q << QDir::toNativeSeparators(ReadyMetaPath());
	args << u"--sha256"_q << staged->sha256;
	args << u"--target"_q << QDir::toNativeSeparators(QDir(cExeDir()).absolutePath());
	args << u"--exe"_q << cExeName();

	auto created = FILETIME();
	auto exited = FILETIME();
	auto kernel = FILETIME();
	auto user = FILETIME();
	if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
		const auto creationTime = (quint64(created.dwHighDateTime) << 32)
			| created.dwLowDateTime;
		args << u"--pid"_q << QString::number(GetCurrentProcessId());
		args << u"--created"_q << QString::number(creationTime);
	}

	auto relaunchArgs = QStringList();
	if (cLaunchMode() == LaunchModeAutoStart) {
		relaunchArgs << u"-autostart"_q;
	}
	if (Logs::DebugEnabled()) {
		relaunchArgs << u"-debug"_q;
	}
	if (cStartInTray()) {
		relaunchArgs << u"-startintray"_q;
	}
	if (customWorkingDir && !workingDir.isEmpty()) {
		relaunchArgs << u"-workdir"_q << workingDir;
	}
	if (!dataFile.isEmpty() && dataFile != u"data"_q) {
		relaunchArgs << u"-key"_q << dataFile;
	}
	if (!relaunchArgs.isEmpty()) {
		args << u"--relaunch-args"_q;
		args << relaunchArgs;
	}

	const auto nativeExe = QDir::toNativeSeparators(runningUpdater);
	auto cmdLine = QuoteWinArg(nativeExe);
	for (const auto &arg : args) {
		cmdLine.append(' ');
		cmdLine.append(QuoteWinArg(arg));
	}
	auto cmdBuf = cmdLine.toStdWString();
	cmdBuf.push_back(L'\0');

	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi = {};
	const auto workDirW = QDir::toNativeSeparators(UpdatesDirPath()).toStdWString();
	if (!CreateProcessW(
			nativeExe.toStdWString().c_str(),
			cmdBuf.data(),
			nullptr,
			nullptr,
			FALSE,
			CREATE_NO_WINDOW,
			nullptr,
			workDirW.c_str(),
			&si,
			&pi)) {
		LOG(("Ovygram Update Error: CreateProcessW failed for OvygramUpdater.exe (err=%1).")
			.arg(GetLastError()));
		return false;
	}
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
#else
	Q_UNUSED(customWorkingDir);
	Q_UNUSED(workingDir);
	Q_UNUSED(dataFile);
	LOG(("Ovygram Update: staged package verified at '%1'.").arg(ReadyZipPath()));
	return false;
#endif // Q_OS_WIN
}

} // namespace Ovygram
