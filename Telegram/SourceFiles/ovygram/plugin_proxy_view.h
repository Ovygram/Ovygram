#pragma once

#include "base/basic_types.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

#include <map>

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Ovygram {

struct ProxyViewState {
	QJsonArray offers;
	QJsonObject saved;
	std::map<QString, QString> active;
};

[[nodiscard]] QString PluginProxyLabel(const QJsonObject &offer);

void SetupPluginProxyBox(
	not_null<Ui::GenericBox*> box,
	int width,
	Fn<ProxyViewState()> query,
	Fn<void(const QString&, const QJsonObject&)> apply);

} // namespace Ovygram
