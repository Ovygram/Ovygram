#include "ovygram/auto_update_view.h"

#include "ovygram/i18n.h"
#include "ovygram/plugin_management_view.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_plugin_management.h"
#include "styles/style_widgets.h"

#include <array>

namespace Ovygram {
namespace {

void AddReadyReleaseCard(
		not_null<Ui::VerticalLayout*> container,
		const UpdateRelease &release) {
	const auto card = AddPluginCard(container);
	const auto versionLine = u"%1-%2 (build #%3)"_q.arg(
		release.ovygramVersion,
		release.telegramVersion,
		QString::number(release.build));
	card->add(
		object_ptr<Ui::FlatLabel>(
			card,
			Tr(u"Ready to install: %1"_q, u"Готово к установке: %1"_q).arg(versionLine),
			st::settingsPluginSectionTitle),
		QMargins(
			st::settingsPluginTextMargin.left(),
			st::settingsPluginSectionMargin.top(),
			st::settingsPluginTextMargin.right(),
			st::settingsPluginLinkTextSkip));

	auto metaParts = QStringList{
		Tr(u"Channel: %1"_q, u"Канал: %1"_q).arg(UpdateChannelTitle(release.sourceChannel)),
	};
	if (!release.commit.isEmpty()) {
		metaParts.push_back(Tr(u"commit %1"_q, u"коммит %1"_q).arg(release.commit.left(7)));
	}
	if (release.size > 0) {
		const auto mb = release.size / (1024.0 * 1024.0);
		metaParts.push_back(Tr(u"%1 MB"_q, u"%1 МБ"_q).arg(QString::number(mb, 'f', 1)));
	}
	card->add(
		object_ptr<Ui::FlatLabel>(
			card,
			metaParts.join(u" · "_q),
			st::settingsPluginCardMeta),
		QMargins(
			st::settingsPluginTextMargin.left(),
			0,
			st::settingsPluginTextMargin.right(),
			release.notes.trimmed().isEmpty()
				? st::settingsPluginSectionMargin.top()
				: st::settingsPluginTextMargin.bottom()));

	if (!release.notes.trimmed().isEmpty()) {
		const auto notesLabel = card->add(
			object_ptr<Ui::FlatLabel>(
				card,
				release.notes.trimmed(),
				st::settingsPluginBody),
			QMargins(
				st::settingsPluginTextMargin.left(),
				0,
				st::settingsPluginTextMargin.right(),
				st::settingsPluginSectionMargin.top()));
		notesLabel->setSelectable(true);
	}
}

} // namespace

void BuildUpdateChannelBox(
		not_null<Ui::GenericBox*> box,
		UpdateChannelBoxData data,
		Fn<void(UpdateChannel)> onSave) {
	box->setTitle(rpl::single(Tr(u"Update channel"_q, u"Канал обновлений"_q)));
	box->setWidth(st::boxWideWidth);

	const auto content = box->verticalLayout();
	if (!data.currentVersionLabel.isEmpty()) {
		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				Tr(u"Current version: %1"_q, u"Текущая версия: %1"_q).arg(data.currentVersionLabel),
				st::settingsPluginCardMeta),
			QMargins(
				st::boxRowPadding.left(),
				0,
				st::boxRowPadding.right(),
				st::settingsPluginSectionMargin.top()));
	}

	const auto group = std::make_shared<Ui::RadiobuttonGroup>(
		static_cast<int>(data.currentChannel));
	constexpr auto kChannels = std::array{
		UpdateChannel::Stable,
		UpdateChannel::Beta,
		UpdateChannel::Canary,
	};
	for (const auto channel : kChannels) {
		const auto radio = content->add(
			object_ptr<Ui::Radiobutton>(
				content,
				group,
				static_cast<int>(channel),
				UpdateChannelTitle(channel),
				st::settingsPluginSelector),
			QMargins(
				st::boxRowPadding.left(),
				st::settingsPluginSelectorMargin.top(),
				st::boxRowPadding.right(),
				st::settingsPluginLinkTextSkip));
		const auto descriptionLeft = st::boxRowPadding.left()
			+ radio->checkRect().width()
			+ st::settingsPluginSelector.textPosition.x()
			- style::ConvertScale(3);
		content->add(
			object_ptr<Ui::FlatLabel>(
				content,
				UpdateChannelDescription(channel),
				st::settingsPluginCardMeta),
			QMargins(
				descriptionLeft,
				0,
				st::boxRowPadding.right(),
				st::settingsPluginSectionMargin.top()));
	}

	if (data.readyRelease) {
		Ui::AddSkip(content, st::settingsPluginLinkTextSkip);
		AddReadyReleaseCard(content, *data.readyRelease);
	}

	box->addButton(rpl::single(Tr(u"Save"_q, u"Сохранить"_q)), [=] {
		const auto chosen = static_cast<UpdateChannel>(group->current());
		if (onSave) {
			onSave(chosen);
		}
		box->closeBox();
	});
	box->addButton(rpl::single(Tr(u"Cancel"_q, u"Отмена"_q)), [=] {
		box->closeBox();
	});
}

void BuildUpdateReadyBox(
		not_null<Ui::GenericBox*> box,
		const UpdateRelease &release,
		Fn<void()> onRestartAndUpdate) {
	box->setTitle(rpl::single(Tr(u"Ovygram Update"_q, u"Обновление Ovygram"_q)));
	box->setWidth(st::boxWidth);

	const auto content = box->verticalLayout();
	AddReadyReleaseCard(content, release);
	content->add(
		object_ptr<Ui::FlatLabel>(
			content,
			Tr(
				u"After restarting, Ovygram will install the update and reopen automatically. "
				u"Your accounts and plugin settings will be preserved."_q,
				u"После перезапуска Ovygram установит обновление и автоматически откроется снова. "
				u"Данные аккаунтов и настройки плагинов сохранятся."_q),
			st::settingsPluginCardMeta),
		QMargins(
			st::boxRowPadding.left(),
			st::settingsPluginTextMargin.bottom(),
			st::boxRowPadding.right(),
			st::settingsPluginTextMargin.bottom()));

	box->addButton(rpl::single(Tr(u"Restart and update"_q, u"Перезапустить и обновить"_q)), [=] {
		if (onRestartAndUpdate) {
			onRestartAndUpdate();
		}
		box->closeBox();
	});
	box->addButton(rpl::single(Tr(u"Later"_q, u"Позже"_q)), [=] {
		box->closeBox();
	});
}

} // namespace Ovygram
