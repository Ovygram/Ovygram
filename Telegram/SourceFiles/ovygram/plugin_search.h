#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <rpl/producer.h>
#include <functional>
#include <vector>

class QWidget;

namespace Ovygram::Search {

struct Scope {
	QWidget *view = nullptr;
	QString account;
	QString chat;
};

struct Query {
	int minDate = 0;
	int maxDate = 0;
	QString media;
	bool conflict = false;
};

void SetEventSender(std::function<void(QJsonObject)> send);
void Handle(const QJsonObject &command);
void RemovePlugin(const QString &installationId);
[[nodiscard]] rpl::producer<Scope> Changes();
[[nodiscard]] rpl::producer<Scope> ContinueRequests();
[[nodiscard]] Query Parameters(const Scope &scope);
[[nodiscard]] bool HasPredicate(const Scope &scope);
// Delivers at most one decision per row, ANDing all matching callbacks.
// rules run in the plugin host. Reply indices refer only to this batch.
void Filter(const Scope &scope, const QJsonArray &rows,
	std::function<void(std::vector<bool>)> done);

} // namespace Ovygram::Search
