#pragma once

#include "base/basic_types.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <rpl/producer.h>

namespace Ui {
class GenericBox;
class VerticalLayout;
} // namespace Ui

namespace Ovygram {

[[nodiscard]] not_null<Ui::VerticalLayout*> AddPluginCard(
	not_null<Ui::VerticalLayout*> layout);
void AddPluginHeader(
	not_null<Ui::VerticalLayout*> card,
	QString title,
	Fn<void()> open,
	bool enabled,
	bool blocked,
	Fn<void()> toggle);
void AddPluginSummary(not_null<Ui::VerticalLayout*> card, const QJsonObject &plugin);
void AddPluginCardActions(
	not_null<Ui::VerticalLayout*> card,
	Fn<void()> share,
	Fn<void()> remove);
void AddPluginDetailsCard(
	not_null<Ui::VerticalLayout*> layout,
	const QJsonObject &plugin,
	bool blocked,
	Fn<void()> toggle);
void AddPluginSettings(
	not_null<Ui::VerticalLayout*> layout,
	const QJsonArray &fields,
	Fn<void(QJsonValue actionId, bool link)> activate,
	Fn<void(QJsonObject)> save);
void AddPluginManagement(
	not_null<Ui::VerticalLayout*> layout,
	const QJsonObject &plugin,
	Fn<void()> updateFile,
	Fn<void(QString)> operate,
	Fn<void()> share,
	Fn<void()> remove);
void BuildPluginRemovalBox(
	not_null<Ui::GenericBox*> box,
	rpl::producer<QString> cancel,
	Fn<void(bool)> remove);

} // namespace Ovygram
