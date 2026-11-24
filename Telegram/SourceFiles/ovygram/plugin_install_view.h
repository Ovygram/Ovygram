#pragma once

#include "base/basic_types.h"
#include "styles/style_plugin_management.h"

#include <QtCore/QJsonObject>
#include <rpl/variable.h>

namespace Ui {
class GenericBox;
class RoundButton;
class VerticalLayout;
} // namespace Ui

namespace Ovygram {

[[nodiscard]] bool IsTrustedPlugin(const QString &status);
[[nodiscard]] not_null<Ui::VerticalLayout*> PreparePluginInstallBox(
	not_null<Ui::GenericBox*> box);
void AddPluginInstallLabel(
	not_null<Ui::VerticalLayout*> rows,
	QString text,
	const style::FlatLabel &style = st::settingsPluginBody);
not_null<Ui::VerticalLayout*> AddPluginInstallCard(
	not_null<Ui::VerticalLayout*> rows,
	const QJsonObject &metadata,
	const QJsonObject &trust);
Ui::RoundButton *SetPluginInstallAction(
	not_null<Ui::GenericBox*> box,
	QString text,
	Fn<void()> callback,
	bool secondary = false);
void AddPluginInstallChoices(
	not_null<Ui::GenericBox*> box,
	not_null<Ui::VerticalLayout*> rows,
	const QJsonObject &info,
	const QString &installationId,
	rpl::variable<bool> &showDetails,
	Fn<void(QString, bool, QJsonObject)> confirm,
	Fn<void(Ui::RoundButton*)> focusCancel);

} // namespace Ovygram
