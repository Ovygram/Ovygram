#pragma once

#include "base/basic_types.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <rpl/producer.h>

namespace Ui {
class InputField;
class RoundButton;
class VerticalLayout;
} // namespace Ui

namespace Ovygram {

[[nodiscard]] QString PluginErrorTitle(const QJsonObject &plugin);
[[nodiscard]] not_null<Ui::InputField*> AddPluginListSearch(
	not_null<Ui::VerticalLayout*> layout);
void AddPluginListNotice(
	not_null<Ui::VerticalLayout*> layout,
	rpl::producer<QString> text);
not_null<Ui::RoundButton*> AddPluginListActions(
	not_null<Ui::VerticalLayout*> layout,
	Fn<void()> import,
	Fn<void()> check);
void AddPluginListRows(
	not_null<Ui::VerticalLayout*> layout,
	const QJsonArray &plugins,
	const QString &query,
	bool launchBlocked,
	Fn<void(QJsonObject)> open,
	Fn<void(QString id, bool enabled)> setEnabled,
	Fn<void(QString id)> applyUpdate,
	Fn<void()> log);

} // namespace Ovygram
