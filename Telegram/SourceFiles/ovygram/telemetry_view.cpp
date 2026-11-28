#include "ovygram/telemetry_view.h"

#include "ovygram/i18n_rpl.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_plugin_management.h"

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace Ovygram::Telemetry {

void AddDiagnosticsRows(
		not_null<Ui::VerticalLayout*> layout,
		QString id,
		bool enabled,
		Fn<void(bool)> setEnabled,
		Fn<void()> preview) {
	const auto toggle = layout->add(object_ptr<Ui::SettingsButton>(
		layout,
		TrValue(u"Send usage statistics and diagnostics"_q, u"Отправлять статистику и диагностику"_q),
		st::settingsPluginSettingRow));
	toggle->setAllowTextLines(2);
	toggle->toggleOn(rpl::single(enabled));
	toggle->toggledChanges() | rpl::on_next(std::move(setEnabled), toggle->lifetime());

	layout->add(object_ptr<Ui::FlatLabel>(layout, Tr(
		u"Ovygram sends your installation ID, app version, platform, openings and failure events to help measure usage and fix issues."_q,
		u"Ovygram отправляет ID установки, версию приложения, платформу, события открытия и сбоев для подсчёта аудитории и исправления ошибок."_q),
		st::settingsPluginCardMeta), st::settingsPluginTextMargin, style::al_justify);
	layout->add(object_ptr<Ui::FlatLabel>(layout, Tr(
		u"Installation ID: %1"_q,
		u"ID установки: %1"_q).arg(id.isEmpty() ? Tr(u"Unavailable"_q, u"Недоступен"_q) : id),
		st::settingsPluginBody), st::settingsPluginSectionMargin, style::al_justify);

	if (!id.isEmpty()) {
		const auto copy = layout->add(object_ptr<Ui::SettingsButton>(layout,
			TrValue(u"Copy installation ID"_q, u"Скопировать ID установки"_q), st::settingsPluginSettingRow));
		copy->setClickedCallback([id = std::move(id)] {
			QGuiApplication::clipboard()->setText(id);
		});
	}
	const auto support = layout->add(object_ptr<Ui::SettingsButton>(layout,
		TrValue(u"Copy details for support"_q, u"Скопировать сведения для поддержки"_q), st::settingsPluginSettingRow));
	support->setClickedCallback(std::move(preview));
}

void BuildSupportBox(not_null<Ui::GenericBox*> box, const QString &details) {
	box->setTitle(TrValue(u"Details for support"_q, u"Сведения для поддержки"_q));
	box->addRow(object_ptr<Ui::FlatLabel>(box, details, st::boxLabel));
	box->addButton(TrValue(u"Copy"_q, u"Скопировать"_q), [=] {
		QGuiApplication::clipboard()->setText(details);
		box->closeBox();
	});
	box->addButton(TrValue(u"Cancel"_q, u"Отмена"_q), [=] { box->closeBox(); });
}

} // namespace Ovygram::Telemetry
