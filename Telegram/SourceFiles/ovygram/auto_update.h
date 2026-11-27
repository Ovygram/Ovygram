#pragma once

#include "base/timer.h"
#include "core/update_checker.h"
#include "ovygram/auto_update_verify.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

#include <memory>
#include <optional>

namespace Ovygram {

class AutoUpdate final {
public:
	using State = Core::UpdateChecker::State;
	using Progress = Core::UpdateChecker::Progress;

	static AutoUpdate &Instance();

	[[nodiscard]] rpl::producer<> checking() const;
	[[nodiscard]] rpl::producer<> isLatest() const;
	[[nodiscard]] rpl::producer<Progress> progress() const;
	[[nodiscard]] rpl::producer<> failed() const;
	[[nodiscard]] rpl::producer<> ready() const;

	void start(bool forceWait = false);
	void stop();

	[[nodiscard]] State state() const;
	[[nodiscard]] int already() const;
	[[nodiscard]] int size() const;
	[[nodiscard]] bool percent() const;

	[[nodiscard]] UpdateChannel channel() const;
	void setChannel(UpdateChannel channel);
	[[nodiscard]] rpl::producer<UpdateChannel> channelChanged() const;

	[[nodiscard]] std::optional<UpdateRelease> readyRelease() const;
	[[nodiscard]] InstalledVersion installedVersion() const;

	[[nodiscard]] static bool CheckReadyUpdate();
	[[nodiscard]] static bool LaunchUpdater(
		bool customWorkingDir,
		const QString &workingDir,
		const QString &dataFile);

private:
	AutoUpdate();
	~AutoUpdate();

	void scheduleNextCheck(bool forceWait);
	void requestManifest();
	void handleManifestReply(QPointer<QNetworkReply> reply, const QUrl &manifestUrl);
	void startDownload(const UpdateRelease &release, const QUrl &manifestUrl);
	void startDownloadRequest(const QUrl &url, int redirectsLeft);
	void handleDownloadReadyRead(QPointer<QNetworkReply> reply);
	void handleDownloadFinished(QPointer<QNetworkReply> reply);
	void failCheck();
	void clearActiveRequests();
	void clearStagedFiles();
	[[nodiscard]] bool loadExistingReadyUpdate();
	void loadChannelSetting();
	void saveChannelSetting(UpdateChannel channel) const;

	base::Timer _timer;
	std::unique_ptr<QNetworkAccessManager> _network;
	QPointer<QNetworkReply> _manifestReply;
	QPointer<QNetworkReply> _downloadReply;

	UpdateChannel _channel = UpdateChannel::Stable;
	bool _channelLoaded = false;
	State _state = State::None;
	int _already = 0;
	int _size = 0;

	std::optional<UpdateRelease> _downloadingRelease;
	QUrl _downloadManifestUrl;
	std::unique_ptr<QFile> _downloadFile;
	std::unique_ptr<QCryptographicHash> _downloadHash;
	qint64 _downloadBytes = 0;
	int _downloadRedirectsLeft = 0;

	std::optional<UpdateRelease> _readyRelease;

	rpl::event_stream<> _checking;
	rpl::event_stream<> _isLatest;
	rpl::event_stream<Progress> _progress;
	rpl::event_stream<> _failed;
	rpl::event_stream<> _ready;
	rpl::event_stream<UpdateChannel> _channelChanged;
};

} // namespace Ovygram
