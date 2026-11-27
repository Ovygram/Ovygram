#pragma once

#include "base/weak_ptr.h"
#include "data/data_types.h"
#include "mtproto/mtp_instance.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <map>
#include <memory>
#include <optional>

class PeerData;
class History;
class HistoryItem;
class QWidget;

namespace Core {
class PluginRuntime;
} // namespace Core

namespace Main {
class Session;
} // namespace Main

namespace Ovygram {
class PluginUi;
class PluginMedia;
} // namespace Ovygram

namespace Api {

// Correspond to desktop._runtime.POINT_BITS. Keep existing indices stable.
enum class PluginEventBit : unsigned {
	Reaction = 10, Read = 11, Topic = 12, Selection = 13,
	DownloadStarted = 14, DownloadFinished = 15, Theme = 16, Language = 17,
};

class PluginClient final : public QObject {
public:
	explicit PluginClient(not_null<Core::PluginRuntime*> runtime);
	~PluginClient();
	void showPluginProxies(Fn<void()> onClosed = {});
	[[nodiscard]] bool observes(PluginEventBit bit) const;
	void postLocalEvent(PluginEventBit bit, QJsonObject event);

	bool beforeRpc(
		MTP::Instance *instance,
		mtpRequestId requestId,
		MTP::details::SerializedRequest &request,
		MTP::ResponseHandler &callbacks);
	bool beforeUpdates(Main::Session &session, MTPUpdates &updates);
	void observeUpdate(Main::Session &session, const MTPUpdate &update);
	void observeUpdates(Main::Session &session, const MTPUpdates &updates);
	void observeMessages(Main::Session &session, const MTPVector<MTPMessage> &messages);
	void observeSentMessage(Main::Session &session, not_null<HistoryItem*> item);
	void chatChanged(Main::Session &session, PeerData *peer);

private:
	struct Pending {
		base::weak_ptr<Main::Session> session;
		QString accountId;
		QString installationId;
		mtpRequestId requestId = 0;
		QJsonArray hookStack;
	};

	void watchAccounts();
	QJsonArray proxyOffers();
	void setProxyRoute(const QString &route, const QJsonObject &offer);
	void updateProxyIndicator();
	std::map<QString, QString> _activeProxyTokens;
	std::map<QString, QString> _activeProxyLabels;
	void synchronize();
	Main::Session *session(const QString &accountId) const;
	Main::Session *session(MTP::Instance *instance) const;
	QJsonValue dispatch(QJsonObject request);
	void receive(const QByteArray &request);
	void reply(const QString &id, QJsonValue result);
	void fail(const QString &id, const QString &name, const QString &description, int code = 0);
	void cancel(const QString &id);
	void rpc(const QJsonObject &request, not_null<Main::Session*> session);
	void emitEvent(QJsonObject event);
	bool afterRpc(
		const QString &accountId,
		mtpRequestId requestId,
		const QString &requestData,
		const QJsonArray &hookStack,
		MTP::Response &response,
		std::optional<MTP::Error> &error);
	void notifyHookError(const QString &text);

	const not_null<Core::PluginRuntime*> _runtime;
	std::unique_ptr<Ovygram::PluginUi> _ui;
	std::unique_ptr<Ovygram::PluginMedia> _media;
	std::map<QString, Pending> _pending;
	std::map<QString, base::weak_ptr<Main::Session>> _sessions;
	rpl::lifetime _accountLifetime;
	rpl::lifetime _lifetime;

};

[[nodiscard]] rpl::producer<QString> PluginProxyIndicatorValue();
bool PluginBeforeRpc(
	MTP::Instance *instance,
	mtpRequestId requestId,
	MTP::details::SerializedRequest &request,
	MTP::ResponseHandler &callbacks);
[[nodiscard]] bool IsPluginRpcCancelled(const MTP::Error &error);
bool PluginBeforeUpdates(Main::Session &session, MTPUpdates &updates);
void PluginObserveUpdate(Main::Session &session, const MTPUpdate &update);
void PluginObserveUpdates(Main::Session &session, const MTPUpdates &updates);
void PluginObserveMessages(Main::Session &session, const MTPVector<MTPMessage> &messages);
void PluginObserveSentMessage(Main::Session &session, not_null<HistoryItem*> item);
void PluginChatChanged(Main::Session &session, PeerData *peer);
void PluginSelectionChanged(History &history, QWidget *window, const MessageIdsList &ids);
void PluginDownloadEvent(not_null<HistoryItem*> item, bool finished, qint64 size);

} // namespace Api
