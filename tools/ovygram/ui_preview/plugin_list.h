#pragma once

#include "base/basic_types.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

namespace Ui {
class InputField;
class VerticalLayout;
} // namespace Ui

namespace PreviewPluginList {

Ui::InputField *AddSearch(not_null<Ui::VerticalLayout*> layout, bool before);
void AddActions(
	not_null<Ui::VerticalLayout*> layout,
	bool before,
	Fn<void()> import,
	Fn<void()> check);
void BuildRows(
	not_null<Ui::VerticalLayout*> layout,
	const QJsonArray &plugins,
	const QString &query,
	bool before,
	Fn<void(QJsonObject)> open,
	Fn<void(QString)> toggle,
	Fn<void()> share,
	Fn<void()> remove,
	Fn<void()> log);

} // namespace PreviewPluginList
