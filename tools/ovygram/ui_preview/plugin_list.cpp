#include "plugin_list.h"

#include "ovygram/plugin_management_view.h"
#include "styles/style_layers.h"
#include "styles/style_plugin_management.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"

namespace PreviewPluginList {
namespace {

int Px(int value) {
	return style::ConvertScale(value);
}

Ui::SettingsButton *Action(
		not_null<Ui::VerticalLayout*> layout,
		QString text,
		Fn<void()> callback,
		const style::SettingsButton &style = st::defaultSettingsButton) {
	const auto button = layout->add(object_ptr<Ui::SettingsButton>(
		layout, rpl::single(std::move(text)), style));
	button->setClickedCallback(std::move(callback));
	return button;
}

QString UpdateLabel(const QJsonObject &plugin) {
	const auto update = plugin.value(u"update"_q).toObject();
	if (update.isEmpty()) {
		return {};
	}
	const auto version = update.value(u"latest_version"_q).toString(update.value(u"version"_q).toString());
	return update.value(u"rollback"_q).toBool()
		? u"Установить безопасную версию %1"_q.arg(version)
		: u"Обновить до версии %1"_q.arg(version);
}

} // namespace

Ui::InputField *AddSearch(not_null<Ui::VerticalLayout*> layout) {
	const auto row = layout->add(object_ptr<Ui::FixedHeightWidget>(layout, Px(46)));
	const auto field = Ui::CreateChild<Ui::InputField>(row, st::defaultMultiSelectSearchField,
		rpl::single(u"Поиск"_q));
	field->setAccessibleName(u"Поиск плагинов"_q);
	const auto clear = Ui::CreateChild<Ui::CrossButton>(row, st::defaultMultiSelectSearchCancel);
	clear->setAccessibleName(u"Очистить поиск"_q);
	clear->setClickedCallback([=] { field->setText({}); });
	clear->toggle(false, anim::type::instant);
	field->changes() | rpl::on_next([=] {
		clear->toggle(!field->getLastText().isEmpty(), anim::type::instant);
	}, clear->lifetime());
	row->sizeValue() | rpl::on_next([=](QSize size) {
		const auto left = Px(40);
		field->resizeToWidth(std::max(1, size.width() - left - clear->width()));
		field->moveToLeft(left, (size.height() - field->height()) / 2, size.width());
		clear->moveToRight(0, (size.height() - clear->height()) / 2, size.width());
	}, row->lifetime());
	row->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(row);
		p.fillRect(0, row->height() - st::lineWidth, row->width(), st::lineWidth, st::shadowFg);
		st::boxFieldSearchIcon.paint(p, Px(8), Px(6), row->width());
	}, row->lifetime());
	return field;
}

void AddActions(
		not_null<Ui::VerticalLayout*> layout,
		Fn<void()> import,
		Fn<void()> check) {
	const auto button = Action(layout, u"Импортировать плагин"_q, std::move(import));
	button->setPaddingOverride({ Px(22), Px(12), Px(22), Px(12) });
	button->setColorOverride(st::lightButtonFg->c);
	Action(layout, u"Проверить обновления"_q, std::move(check));
}

void BuildRows(
		not_null<Ui::VerticalLayout*> layout,
		const QJsonArray &plugins,
		const QString &query,
		Fn<void(QJsonObject)> open,
		Fn<void(QString id, bool enabled)> setEnabled,
		Fn<void()> share,
		Fn<void()> remove,
		Fn<void()> log) {
	Ui::AddSkip(layout);
	Ui::AddDivider(layout);
	Ui::AddSkip(layout);
	Ui::AddSubsectionTitle(layout, rpl::single(u"Установленные плагины"_q));
	auto found = false;
	for (const auto &entry : plugins) {
		const auto plugin = entry.toObject();
		const auto name = plugin.value(u"name"_q).toString();
		if (!name.contains(query, Qt::CaseInsensitive)
			&& !plugin.value(u"plugin_id"_q).toString().contains(query, Qt::CaseInsensitive)
			&& !plugin.value(u"author"_q).toString().contains(query, Qt::CaseInsensitive)) {
			continue;
		}
		found = true;
		const auto id = plugin.value(u"installation_id"_q).toString();
		const auto enabled = plugin.value(u"enabled"_q).toBool();
		const auto openRow = [=] { open(plugin); };
		const auto card = Ovygram::AddPluginCard(layout);
		const auto blocked = plugin.value(u"trust"_q).toObject().value(u"status"_q).toString() == u"blocked"_q;
		Ovygram::AddPluginHeader(card, name, openRow, enabled, blocked, [=] { setEnabled(id, !enabled); });
		Ovygram::AddPluginSummary(card, plugin);
		if (const auto update = UpdateLabel(plugin); !update.isEmpty()) {
			Action(card, update, openRow, st::settingsPluginCardAction);
		}
		Ovygram::AddPluginCardActions(card, share, remove);
	}
	if (!found) {
		Ui::AddDividerText(layout, rpl::single(plugins.isEmpty()
			? u"Установленных плагинов пока нет."_q : u"Плагины не найдены."_q));
	}
	Ui::AddSkip(layout);
	Ui::AddDivider(layout);
	Action(layout, u"Открыть журнал"_q, std::move(log));
	Ui::AddSkip(layout);
}

} // namespace PreviewPluginList
