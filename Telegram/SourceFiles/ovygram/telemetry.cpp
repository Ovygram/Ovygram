#include "ovygram/telemetry.h"

#include "ovygram/auto_update.h"
#include "ovygram/i18n.h"
#include "ovygram/telemetry_crash_marker.h"
#include "settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLockFile>
#include <QtCore/QSaveFile>
#include <QtCore/QTimer>
#include <QtCore/QUuid>

#include <algorithm>

namespace Ovygram::Telemetry {
namespace {

constexpr auto kEndpoint = "https://api.ovygram.opik.net/v1/telemetry/events";
constexpr auto kBatchSize = 32;
constexpr auto kMaxQueued = 256;
constexpr auto kMaxEventAgeDays = 30;
constexpr auto kFirstRetryMs = 30'000;
constexpr auto kMaxRetryMs = 60 * 60 * 1000;

QString NewId() {
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString Directory() {
	return cWorkingDir() + u"tdata/"_q;
}

QString FilePath() {
	return Directory() + u"ovygram_diagnostics.json"_q;
}

#ifdef TDESKTOP_DISABLE_CRASH_REPORTS
QString CrashMarkerPath() {
	return Directory() + u"ovygram_fatal_exit"_q;
}
#endif // TDESKTOP_DISABLE_CRASH_REPORTS

QJsonObject Read() {
	auto file = QFile(FilePath());
	return file.open(QIODevice::ReadOnly)
		? QJsonDocument::fromJson(file.read(128 * 1024)).object()
		: QJsonObject();
}

QJsonObject Event(const QString &kind) {
	const auto version = AutoUpdate::Instance().installedVersion();
	return {
		{ u"id"_q, NewId() },
		{ u"occurred_at"_q, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) },
		{ u"kind"_q, kind },
		{ u"platform"_q, CurrentUpdatePlatformKey() },
		{ u"version"_q, version.ovygramVersion },
		{ u"build"_q, std::max(0, version.build) },
		{ u"channel"_q, UpdateChannelName(AutoUpdate::Instance().channel()) },
	};
}

class Profile final {
public:
	void start(bool confirmedPreviousCrash) {
		if (_lock || cTestAgent()) {
			return;
		}
		QDir().mkpath(Directory());
		auto lock = std::make_unique<QLockFile>(FilePath() + u".lock"_q);
		lock->setStaleLockTime(0);
		if (!lock->tryLock(0)) {
			return;
		}
		_lock = std::move(lock);
		auto data = Read();
		_id = data.value(u"installation_id"_q).toString();
		if (QUuid(_id).version() != QUuid::Random) {
			_id = NewId();
			data = QJsonObject();
		}
		_token = data.value(u"installation_token"_q).toString();
		if (QUuid(_token).version() != QUuid::Random) {
			_token = NewId();
		}
		_enabled = data.value(u"enabled"_q).toBool(true);
		if (_enabled) {
#ifdef TDESKTOP_DISABLE_CRASH_REPORTS
			confirmedPreviousCrash = confirmedPreviousCrash || QFileInfo(CrashMarkerPath()).size() > 0;
#endif // TDESKTOP_DISABLE_CRASH_REPORTS
			_queue = data.value(u"queue"_q).toArray();
			auto previous = data.value(u"previous_exit"_q).toObject();
			if (!previous.isEmpty()) {
				previous.insert(u"kind"_q, confirmedPreviousCrash ? u"client_crash"_q : u"unclean_exit"_q);
				previous.insert(u"occurred_at"_q, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
				append(previous);
			}
			beginSession();
		}
		persist();
	}

	void finish() {
		if (!_lock) {
			return;
		}
		stopTransport();
		disarmCrashMarker();
		_previousExit = QJsonObject();
		persist();
		_lock.reset();
	}

	void windowOpened() {
		if (!_lock) {
			return;
		}
		_windowOpened = true;
		if (!_lastOpen.isValid() || _lastOpen.elapsed() >= 1000) {
			_lastOpen.start();
			record(u"window_opened"_q);
		}
	}

	void hostStarted() {
		_hostRunning = true;
		record(u"plugin_host_started"_q);
	}

	void hostFailed() {
		if (_hostRunning) {
			_hostRunning = false;
			record(u"plugin_host_failed"_q);
		}
	}

	bool enabled() const {
		return _lock ? _enabled : Read().value(u"enabled"_q).toBool(true);
	}

	QString id() const {
		return _lock ? _id : Read().value(u"installation_id"_q).toString();
	}

	void setEnabled(bool enabled) {
		if (!_lock || _enabled == enabled) {
			return;
		}
		_enabled = enabled;
		if (_enabled) {
			beginSession();
			if (_hostRunning) {
				append(Event(u"plugin_host_started"_q));
			}
			if (_windowOpened) {
				append(Event(u"window_opened"_q));
			}
		} else {
			stopTransport();
			disarmCrashMarker();
			_queue = QJsonArray();
			_previousExit = QJsonObject();
		}
		persist();
		schedule();
	}

private:
	void beginSession() {
		_previousExit = Event(u"unclean_exit"_q);
		append(Event(u"app_started"_q));
#ifdef TDESKTOP_DISABLE_CRASH_REPORTS
		CrashMarker::Arm(CrashMarkerPath());
#endif // TDESKTOP_DISABLE_CRASH_REPORTS
	}

	void disarmCrashMarker() {
#ifdef TDESKTOP_DISABLE_CRASH_REPORTS
		CrashMarker::Disarm();
		QFile::remove(CrashMarkerPath());
#endif // TDESKTOP_DISABLE_CRASH_REPORTS
	}

	void append(const QJsonObject &event) {
		_queue.append(event);
		while (_queue.size() > kMaxQueued) {
			_queue.removeFirst();
		}
	}

	void record(const QString &kind) {
		if (!_lock || !_enabled) {
			return;
		}
		append(Event(kind));
		persist();
		schedule();
	}

	void persist() const {
		auto file = QSaveFile(FilePath());
		if (!file.open(QIODevice::WriteOnly)) {
			return;
		}
		file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
		const auto data = QJsonDocument(QJsonObject{
			{ u"installation_id"_q, _id },
			{ u"installation_token"_q, _token },
			{ u"enabled"_q, _enabled },
			{ u"queue"_q, _queue },
			{ u"previous_exit"_q, _previousExit },
		}).toJson(QJsonDocument::Compact);
		if (file.write(data) == data.size()) {
			file.commit();
		}
	}

	void stopTransport() {
		_timer.reset();
		if (_reply) {
			QObject::disconnect(_reply, nullptr, _network.get(), nullptr);
			_reply->abort();
			_reply = nullptr;
		}
		_network.reset();
		_retryMs = kFirstRetryMs;
	}

	void schedule(int delay = 250) {
		if (!_enabled || !_windowOpened || _queue.isEmpty() || _reply) {
			return;
		}
		if (!_timer) {
			_timer = std::make_unique<QTimer>();
			_timer->setSingleShot(true);
			QObject::connect(_timer.get(), &QTimer::timeout, [this] { send(); });
		}
		if (!_timer->isActive()) {
			_timer->start(delay);
		}
	}

	void send() {
		const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-kMaxEventAgeDays);
		auto batch = QJsonArray();
		auto valid = QJsonArray();
		for (const auto &entry : _queue) {
			const auto event = entry.toObject();
			if (QDateTime::fromString(event.value(u"occurred_at"_q).toString(), Qt::ISODateWithMs) >= cutoff) {
				valid.append(event);
				if (batch.size() < kBatchSize) {
					batch.append(event);
				}
			}
		}
		_queue = std::move(valid);
		if (batch.isEmpty()) {
			persist();
			return;
		}
		if (!_network) {
			_network = std::make_unique<QNetworkAccessManager>();
		}
		auto request = QNetworkRequest(QUrl(QString::fromLatin1(kEndpoint)));
		request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_q);
		request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
		request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
		request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
		request.setTransferTimeout(10'000);
		const auto data = QJsonDocument(QJsonObject{
			{ u"installation_id"_q, _id },
			{ u"installation_token"_q, _token },
			{ u"events"_q, batch },
		}).toJson(QJsonDocument::Compact);
		const auto reply = _network->post(request, data);
		_reply = reply;
		QObject::connect(reply, &QNetworkReply::finished, _network.get(), [=, this] {
			const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			_reply = nullptr;
			reply->deleteLater();
			if (status == 204 || status == 400 || status == 422) {
				auto remaining = QJsonArray();
				for (const auto &entry : _queue) {
					if (!batch.contains(entry)) {
						remaining.append(entry);
					}
				}
				_queue = std::move(remaining);
				persist();
				_retryMs = kFirstRetryMs;
				schedule();
			} else {
				schedule(_retryMs);
				_retryMs = std::min(_retryMs * 2, kMaxRetryMs);
			}
		});
	}

	std::unique_ptr<QLockFile> _lock;
	QString _id;
	QString _token;
	QJsonArray _queue;
	QJsonObject _previousExit;
	bool _enabled = true;
	bool _windowOpened = false;
	bool _hostRunning = false;
	QElapsedTimer _lastOpen;
	std::unique_ptr<QTimer> _timer;
	std::unique_ptr<QNetworkAccessManager> _network;
	QPointer<QNetworkReply> _reply;
	int _retryMs = kFirstRetryMs;
};

Profile &Instance() {
	static auto result = Profile();
	return result;
}

} // namespace

void Start(bool confirmedPreviousCrash) { Instance().start(confirmedPreviousCrash); }
void Finish() { Instance().finish(); }
void WindowOpened() { Instance().windowOpened(); }
void PluginHostStarted() { Instance().hostStarted(); }
void PluginHostFailed() { Instance().hostFailed(); }
bool Enabled() { return Instance().enabled(); }
void SetEnabled(bool enabled) { Instance().setEnabled(enabled); }
QString InstallationId() { return Instance().id(); }

QString SupportDetails() {
	const auto version = AutoUpdate::Instance().installedVersion();
	const auto channel = UpdateChannelName(AutoUpdate::Instance().channel());
	return Tr(
		u"Installation ID: %1\nOvygram: %2 (build %3)\nPlatform: %4\nUpdate channel: %5"_q,
		u"ID установки: %1\nOvygram: %2 (сборка %3)\nПлатформа: %4\nКанал обновлений: %5"_q
	).arg(InstallationId(), version.ovygramVersion, QString::number(version.build), CurrentUpdatePlatformKey(), channel);
}

} // namespace Ovygram::Telemetry
