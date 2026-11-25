#include "ovygram/plugin_media.h"

#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/file_utilities.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_download_manager.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "ovygram/plugin_install.h"
#include "ovygram/protected_ui.h"

#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QPointer>
#include <QtCore/QUuid>
#include <QtCore/QTimer>
#include <QtWidgets/QWidget>
#include <algorithm>
#include <stdexcept>

namespace Ovygram {
namespace {
bool PickerOpen = false;
constexpr auto kReadSize = 256 * 1024;

QJsonValue Playback(not_null<Main::Session*> session, const QJsonObject &command) {
	const auto kind = command.value(u"kind"_q).toString();
	if (kind != u"song"_q && kind != u"voice"_q) throw std::runtime_error("Invalid playback type.");
	const auto type = (kind == u"song"_q) ? AudioMsgId::Type::Song : AudioMsgId::Type::Voice;
	const auto player = Media::Player::instance();
	const auto current = player->current(type);
	const auto document = current.audio();
	const auto state = player->getState(type);
	const auto query = command.value(u"method"_q).toString() == u"media.playback"_q;
	if (!document || &document->session() != session.get() || state.id != current) {
		if (query) return QJsonValue(QJsonValue::Null);
		throw std::runtime_error("Track is no longer current for this account.");
	}
	const auto fullId = current.contextId();
	const auto chatId = QString::number(fullId.peer.value);
	const auto documentId = QString::number(document->id);
	const auto frequency = std::max(state.frequency, 1);
	if (query) {
		return QJsonObject{
			{ u"kind"_q, kind }, { u"chat_id"_q, chatId },
			{ u"message_id"_q, qint64(fullId.msg.bare) }, { u"document_id"_q, documentId },
			{ u"position_ms"_q, qint64(1000 * state.position / frequency) },
			{ u"duration_ms"_q, qint64(1000 * state.length / frequency) },
			{ u"playing"_q, Media::Player::ShowPauseIcon(state.state) },
			{ u"buffering"_q, state.waitingForData },
		};
	}
	if (command.value(u"document_id"_q).toString() != documentId
		|| command.value(u"chat_id"_q).toString() != chatId
		|| command.value(u"message_id"_q).toInteger(-1) != fullId.msg.bare) {
		throw std::runtime_error("Playback selection changed.");
	}
	const auto action = command.value(u"action"_q).toString();
	if (action == u"pause"_q) player->pause(type);
	else if (action == u"play"_q) player->play(type);
	else if (action == u"stop"_q) player->stop(type);
	else if (action == u"next"_q) return player->next(type);
	else if (action == u"previous"_q) return player->previous(type);
	else if (action == u"seek"_q) {
		const auto ms = command.value(u"position_ms"_q).toInteger(-1);
		if (ms < 0 || state.length <= 0 || player->isSeeking(type)) {
			throw std::runtime_error("Track is unavailable for seeking.");
		}
		const auto fraction = std::clamp(double(ms) * frequency / 1000. / state.length, 0., 1.);
		player->startSeeking(type);
		player->finishSeeking(type, fraction);
	} else {
		throw std::runtime_error("Unknown player command.");
	}
	return true;
}
} // namespace

struct PluginMedia::Grant {
	base::weak_ptr<Main::Session> session;
	QString owner;
	crl::time lastRead = crl::now();
	std::unique_ptr<QIODevice> device;
	qint64 size = 0;
};

PluginMedia::PluginMedia() {
	const auto timer = new QTimer(this);
	timer->setInterval(60000);
	connect(timer, &QTimer::timeout, this, [=] {
		const auto now = crl::now();
		std::erase_if(_grants, [=](const auto &entry) {
			return !entry.second->session || now - entry.second->lastRead > 120000;
		});
	});
	timer->start();
}
PluginMedia::~PluginMedia() = default;

bool PluginFilePickerActive() {
	return PickerOpen;
}

void PluginMedia::pick(
		not_null<Main::Session*> session,
		QString installationId,
		QString name,
		Fn<bool()> pending,
		Result reply,
		Error fail) {
	if (InstallationUiActive() || ProtectedSurfaceExists() || Core::App().passcodeLocked()) {
		throw std::runtime_error("File selection is currently unavailable.");
	}
	const auto owned = std::count_if(_grants.begin(), _grants.end(), [&](const auto &entry) {
		return entry.second->owner == installationId;
	});
	if (owned >= 4 || _grants.size() >= 32) throw std::runtime_error("Too many concurrent file transfers.");
	const auto weak = base::make_weak(session.get());
	const auto guard = QPointer<PluginMedia>(this);
	PickerOpen = true;
	FileDialog::GetOpenPathChecked(Core::App().getFileDialogParent(),
		u"Передать копию файла плагину "_q + name + u" · "_q + installationId.left(8),
		FileDialog::AllFilesFilter(),
		[=](FileDialog::OpenResult &&result) {
			PickerOpen = false;
			if (!guard || !pending()) return;
			if (!weak || Core::App().passcodeLocked()
				|| ProtectedSurfaceExists() || InstallationUiActive()) {
				fail(u"Аккаунт закрыт или приложение заблокировано."_q);
				return;
			}
			const auto currentOwned = std::count_if(
				guard->_grants.begin(),
				guard->_grants.end(),
				[&](const auto &entry) {
					return entry.second->owner == installationId;
				});
			if (currentOwned >= 4 || guard->_grants.size() >= 32) {
				fail(u"Слишком много активных передач файлов."_q);
				return;
			}
			auto grant = std::make_unique<Grant>();
			const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
			grant->owner = installationId;
			grant->session = weak;
			auto filename = QString();
			if (!result.remoteContent.isEmpty()) {
				auto buffer = std::make_unique<QBuffer>();
				buffer->setData(result.remoteContent);
				grant->device = std::move(buffer);
				filename = QFileInfo(result.paths.value(0)).fileName();
			} else {
				const auto info = QFileInfo(result.paths.value(0));
				if (info.isFile()) grant->device = std::make_unique<QFile>(info.absoluteFilePath());
				filename = info.fileName();
			}
			if (!grant->device || !grant->device->open(QIODevice::ReadOnly) || grant->device->isSequential()) {
				fail(u"Не удалось открыть выбранный обычный файл."_q);
				return;
			}
			grant->size = grant->device->size();
			const auto descriptor = QJsonObject{
				{ u"token"_q, token }, { u"name"_q, filename },
				{ u"size"_q, grant->size },
			};
			guard->_grants[token] = std::move(grant);
			reply(descriptor);
		}, [=] {
			PickerOpen = false;
			if (!guard || !pending()) return;
			if (!weak || Core::App().passcodeLocked() || ProtectedSurfaceExists()) {
				fail(u"Выбор файла сейчас недоступен."_q);
				return;
			}
			reply(QJsonValue(QJsonValue::Null));
		}, [=] {
			return guard && weak && pending() && !Core::App().passcodeLocked()
				&& !ProtectedSurfaceExists();
		});
}

QJsonValue PluginMedia::request(not_null<Main::Session*> session, const QJsonObject &command) {
	const auto method = command.value(u"method"_q).toString();
	const auto cancel = (method == u"media.cancel_download"_q);
	if (method == u"media.downloads"_q || cancel) {
		auto result = QJsonArray();
		for (const auto entry : Core::App().downloadManager().loadingList()) {
			const auto item = entry->object.item;
			if (&item->history()->session() != session.get() || entry->hiddenByView) continue;
			const auto fullId = item->fullId();
			if (cancel) {
				if (entry->done || command.value(u"chat_id"_q).toString() != QString::number(fullId.peer.value)
					|| command.value(u"message_id"_q).toInteger(-1) != fullId.msg.bare
					|| command.value(u"started"_q).toInteger(-1) != entry->started) continue;
				if (entry->externalCancel) {
					Core::App().downloadManager().cancelLoadingExternal(item);
				} else if (const auto document = entry->object.document) {
					document->cancel();
				} else if (const auto photo = entry->object.photo) {
					photo->cancel();
				} else {
					return false;
				}
				return true;
			}
			result.push_back(QJsonObject{
				{ u"chat_id"_q, QString::number(fullId.peer.value) },
				{ u"message_id"_q, qint64(fullId.msg.bare) },
				{ u"started"_q, qint64(entry->started) },
				{ u"transferred"_q, qint64(entry->ready) },
				{ u"size"_q, qint64(entry->total) }, { u"done"_q, entry->done },
			});
		}
		if (cancel) return false;
		return result;
	} else if (method == u"media.playback"_q || method == u"media.control"_q) {
		return Playback(session, command);
	}
	const auto i = _grants.find(command.value(u"token"_q).toString());
	if (i == end(_grants) || i->second->session.get() != session.get()
		|| i->second->owner != command.value(u"installation_id"_q).toString()) {
		throw std::runtime_error("Selected file is unavailable.");
	}
	auto &grant = *i->second;
	grant.lastRead = crl::now();
	if (method == u"media.release"_q) {
		_grants.erase(i);
		return true;
	} else if (method == u"media.read"_q) {
		const auto count = std::min<qint64>(kReadSize, grant.size - grant.device->pos());
		if (count <= 0) throw std::runtime_error("Selected file is exhausted.");
		const auto bytes = grant.device->read(count);
		if (bytes.size() != count) throw std::runtime_error("Selected file changed.");
		return QString::fromLatin1(bytes.toBase64());
	}
	throw std::runtime_error("Unknown media operation.");
}

void PluginMedia::remove(const QString &installationId) {
	std::erase_if(_grants, [&](const auto &entry) { return entry.second->owner == installationId; });
}
} // namespace Ovygram
