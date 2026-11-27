#include "ovygram/plugin_management_view.h"

#include "ovygram/i18n.h"
#include "ovygram/plugin_trust_badge.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_plugin_management.h"

#include <algorithm>
#include <vector>

namespace Ovygram {
namespace {

[[nodiscard]] QString PluginStatus(const QJsonObject &plugin) {
	return plugin.value(u"loaded"_q).toBool()
		? Tr(u"Running"_q, u"Запущен"_q)
		: plugin.value(u"enabled"_q).toBool()
		? Tr(u"Enabled, not running"_q, u"Включён, но не запущен"_q)
		: Tr(u"Stopped"_q, u"Остановлен"_q);
}

[[nodiscard]] QString PluginMetadata(const QJsonObject &plugin) {
	auto result = Tr(u"Version %1 · %2 · %3"_q, u"Версия %1 · %2 · %3"_q).arg(
		plugin.value(u"version"_q).toString().simplified(),
		plugin.value(u"display_author"_q).toString(plugin.value(u"author"_q).toString()).simplified(),
		plugin.value(u"plugin_id"_q).toString());
	// Installations of one plugin ID differ by their installation_id.
	if (plugin.value(u"instances"_q).toInt() > 1) {
		result += Tr(u" · instance %1"_q, u" · экземпляр %1"_q).arg(
			plugin.value(u"installation_id"_q).toString().left(8));
	}
	return result;
}

[[nodiscard]] QString TrustStatus(const QJsonObject &plugin) {
	const auto status = plugin.value(u"trust"_q).toObject().value(u"status"_q).toString();
	if (status == u"verified"_q) return Tr(u"Verified plugin"_q, u"Проверенный плагин"_q);
	if (status == u"official"_q) return Tr(u"Official plugin"_q, u"Официальный плагин"_q);
	if (status == u"blocked"_q) return Tr(u"Dangerous plugin / malware — launch blocked"_q, u"Опасный плагин / вирус — запуск запрещён"_q);
	if (status == u"revoked"_q) return Tr(u"Unverified plugin — verification revoked"_q, u"Непроверенный плагин — проверка отозвана"_q);
	if (status == u"verification_unavailable"_q) return Tr(u"Verification unavailable"_q, u"Проверка недоступна"_q);
	return Tr(u"Unverified plugin"_q, u"Непроверенный плагин"_q);
}

not_null<Ui::SettingsButton*> AddAction(
		not_null<Ui::VerticalLayout*> layout,
		QString text,
		Fn<void()> callback,
		const style::SettingsButton &style = st::settingsPluginSettingRow) {
	const auto button = layout->add(object_ptr<Ui::SettingsButton>(
		layout,
		rpl::single(std::move(text)),
		style));
	button->setClickedCallback(std::move(callback));
	return button;
}

} // namespace

not_null<Ui::VerticalLayout*> AddPluginCard(
		not_null<Ui::VerticalLayout*> layout) {
	const auto card = layout->add(
		object_ptr<Ui::VerticalLayout>(layout),
		st::settingsPluginCardMargin,
		style::al_justify);
	card->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(card);
		const auto quality = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::settingsPluginCardFill);
		const auto radius = st::settingsPluginCardRadius;
		p.drawRoundedRect(card->rect(), radius, radius);
	}, card->lifetime());
	return card;
}

namespace {

not_null<Ui::FlatLabel*> AddPluginLabel(
		not_null<Ui::VerticalLayout*> layout,
		QString text,
		const style::FlatLabel &style,
		style::margins margin = st::settingsPluginTextMargin) {
	auto label = object_ptr<Ui::FlatLabel>(layout, std::move(text), style);
	return layout->add(std::move(label), margin, style::al_justify);
}

class PluginMetadataRow final : public Ui::RpWidget {
public:
	using Ui::RpWidget::RpWidget;

	void add(object_ptr<Ui::VerticalLayout> badge) {
		const auto child = badge.release();
		child->show();
		_badges.push_back(child);
	}

protected:
	int resizeGetHeight(int width) override {
		auto left = 0;
		auto top = 0;
		auto rowHeight = 0;
		const auto gap = st::settingsPluginBadgeGap;
		for (const auto badge : _badges) {
			const auto chipWidth = std::min(width, badge->naturalWidth());
			if (left && left + chipWidth > width) {
				top += rowHeight + gap;
				left = 0;
				rowHeight = 0;
			}
			badge->resizeToWidth(chipWidth);
			badge->moveToLeft(left, top, width);
			left += chipWidth + gap;
			rowHeight = std::max(rowHeight, badge->height());
		}
		return top + rowHeight;
	}

private:
	std::vector<not_null<Ui::VerticalLayout*>> _badges;
};

} // namespace

void AddPluginSummary(
		not_null<Ui::VerticalLayout*> card,
		const QJsonObject &plugin) {
	AddPluginLabel(card, PluginMetadata(plugin), st::settingsPluginCardMeta);
	if (const auto description = I18n::PluginDescription(plugin); !description.isEmpty()) {
		AddPluginLabel(card, description, st::settingsPluginBody)->setSelectable(true);
	}
	const auto trust = plugin.value(u"trust"_q).toObject();
	const auto status = trust.value(u"status"_q).toString();
	auto chips = object_ptr<PluginMetadataRow>(card);
	chips->add(Ovygram::MakePluginBadge(chips, TrustStatus(plugin), status));
	for (const auto &value : plugin.value(u"requirements"_q).toArray()) {
		if (auto name = value.toString(); !name.isEmpty()) {
			chips->add(Ovygram::MakePluginBadge(chips, std::move(name)));
		}
	}
	card->add(std::move(chips), st::settingsPluginTextMargin, style::al_justify);
	if (status == u"verification_unavailable"_q) {
		if (const auto reason = trust.value(u"reason"_q).toString(); !reason.isEmpty()) {
			AddPluginLabel(card, reason, st::settingsPluginCardMeta);
		}
	}
	if (plugin.value(u"installing"_q).toBool()) {
		AddPluginLabel(card, Tr(u"Installing dependencies…"_q, u"Устанавливаются зависимости…"_q), st::settingsPluginCardMeta);
	}
}

namespace {

class PluginCardButton final : public Ui::SettingsButton {
public:
	PluginCardButton(QWidget *parent, QString text, const style::SettingsButton &style, bool toggle)
	: Ui::SettingsButton(parent, rpl::single(toggle ? QString() : std::move(text)), style)
	, _toggle(toggle) {
		if (_toggle) {
			setAccessibleName(std::move(text));
		}
	}

	QString accessibilityName() override {
		return _toggle ? accessibleName() : Ui::SettingsButton::accessibilityName();
	}

protected:
	void paintEvent(QPaintEvent *event) override {
		if (!_toggle && (isOver() || isDown()) && !isDisabled()) {
			auto p = QPainter(this);
			const auto quality = PainterHighQualityEnabler(p);
			auto fill = st::settingsPluginCardActionOverlay->c;
			fill.setAlpha(isDown() ? st::settingsPluginCardActionPressedAlpha
				: st::settingsPluginCardActionHoverAlpha);
			p.setPen(Qt::NoPen);
			p.setBrush(fill);
			const auto bounds = QRect(4, 4, std::min(width() - 8, fullTextWidth() + 20), height() - 8);
			const auto radius = st::settingsPluginLegacyCardActionRadius;
			p.drawRoundedRect(bounds, radius, radius);
		}
		Ui::SettingsButton::paintEvent(event);
	}

private:
	const bool _toggle;
};

class PluginButtonRow final : public Ui::RpWidget {
public:
	PluginButtonRow(QWidget *parent, bool hasToggle = false)
	: Ui::RpWidget(parent)
	, _hasToggle(hasToggle) {
	}

	not_null<Ui::SettingsButton*> addButton(
			QString text,
			const style::SettingsButton &style,
			Fn<void()> action = nullptr) {
		const auto button = Ui::CreateChild<PluginCardButton>(
			this,
			std::move(text),
			style,
			_hasToggle && !_buttons.empty());
		if (action) {
			button->setClickedCallback(std::move(action));
		}
		button->show();
		_buttons.push_back(button);
		return button;
	}

	void addStatus(QString text, bool running) {
		const auto status = Ui::CreateChild<Ui::VerticalLayout>(this);
		const auto diameter = st::settingsPluginStatusDot;
		const auto left = st::settingsPluginTextMargin.left();
		auto padding = st::settingsPluginCardTitle.padding;
		padding.setLeft(left + diameter + st::settingsPluginStatusSkip);
		AddPluginLabel(status, std::move(text), st::settingsPluginStatus, padding);
		status->paintRequest() | rpl::on_next([=] {
			auto p = QPainter(status);
			const auto quality = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(running
				? Ovygram::PluginTrustColor(u"verified"_q)
				: st::windowSubTextFg->c);
			p.drawEllipse(style::rtlrect(left, (status->height() - diameter) / 2,
				diameter, diameter, status->width()));
		}, status->lifetime());
		status->show();
		_buttons.push_back(status);
	}

protected:
	int resizeGetHeight(int width) override {
		const auto count = int(_buttons.size());
		const auto toggleWidth = _hasToggle
			? std::min(width, st::settingsPluginToggleWidth)
			: 0;
		const auto regular = count - (_hasToggle ? 1 : 0);
		const auto space = width - toggleWidth;
		auto left = 0;
		auto height = 0;
		for (auto i = 0; i != count; ++i) {
			const auto right = (i < regular) ? (space * (i + 1) / regular) : width;
			_buttons[i]->resizeToWidth(right - left);
			height = std::max(height, _buttons[i]->height());
			_buttons[i]->moveToLeft(left, 0, width);
			left = right;
		}
		return height;
	}

private:
	const bool _hasToggle;
	std::vector<not_null<Ui::RpWidget*>> _buttons;
};

void AddPluginToggle(
		not_null<PluginButtonRow*> row,
		bool enabled,
		bool blocked,
		Fn<void()> toggle) {
	const auto button = row->addButton(
		enabled
			? Tr(u"Disable plugin"_q, u"Отключить плагин"_q)
			: Tr(u"Enable plugin"_q, u"Включить плагин"_q),
		st::settingsPluginToggleAction,
		std::move(toggle));
	button->toggleOn(rpl::single(enabled), true);
	button->setDisabled(blocked && !enabled);
}

} // namespace

void AddPluginHeader(
		not_null<Ui::VerticalLayout*> card,
		QString title,
		Fn<void()> open,
		bool enabled,
		bool blocked,
		Fn<void()> toggle) {
	auto row = object_ptr<PluginButtonRow>(card, true);
	row->addButton(std::move(title), st::settingsPluginCardTitle, std::move(open));
	AddPluginToggle(row.data(), enabled, blocked, std::move(toggle));
	card->add(std::move(row));
}

void AddPluginDetailsCard(
		not_null<Ui::VerticalLayout*> layout,
		const QJsonObject &plugin,
		bool blocked,
		Fn<void()> toggle) {
	const auto card = AddPluginCard(layout);
	auto row = object_ptr<PluginButtonRow>(card, true);
	row->addStatus(PluginStatus(plugin), plugin.value(u"loaded"_q).toBool());
	AddPluginToggle(row.data(), plugin.value(u"enabled"_q).toBool(), blocked,
		std::move(toggle));
	card->add(std::move(row));
	AddPluginSummary(card, plugin);
}

void AddPluginCardActions(
		not_null<Ui::VerticalLayout*> card,
		Fn<void()> share,
		Fn<void()> remove) {
	auto row = object_ptr<PluginButtonRow>(card);
	row->addButton(Tr(u"Share"_q, u"Отправить"_q), st::settingsPluginCardAction, std::move(share));
	row->addButton(Tr(u"Remove"_q, u"Удалить"_q), st::settingsPluginCardRemove, std::move(remove));
	card->add(std::move(row));
}

namespace {

void AddDetailsSection(not_null<Ui::VerticalLayout*> layout, QString title) {
	Ui::AddSkip(layout);
	Ui::AddDivider(layout);
	AddPluginLabel(layout, std::move(title), st::settingsPluginSectionTitle,
		st::settingsPluginSectionMargin);
}

void AddRowIcon(
		not_null<Ui::SettingsButton*> button,
		const style::SettingsButton &st,
		const style::icon &icon) {
	const auto widget = Ui::CreateChild<Ui::RpWidget>(button.get());
	widget->setAttribute(Qt::WA_TransparentForMouseEvents);
	widget->resize(icon.size());
	widget->show();
	button->sizeValue() | rpl::on_next([=, left = st.iconLeft](QSize size) {
		widget->moveToLeft(left, (size.height() - widget->height()) / 2, size.width());
	}, widget->lifetime());
	widget->paintRequest() | rpl::on_next([=, &icon] {
		auto painter = QPainter(widget);
		icon.paint(painter, 0, 0, widget->width());
	}, widget->lifetime());
}

void AddMenuAction(
		not_null<Ui::VerticalLayout*> layout,
		QString text,
		const style::icon &icon,
		Fn<void()> action,
		const style::SettingsButton &style = st::settingsPluginMenuRow) {
	const auto button = AddAction(layout, std::move(text), std::move(action), style);
	button->setAllowTextLines();
	AddRowIcon(button, style, icon);
}

class PluginSettingsLink final : public Ui::SettingsButton {
public:
	PluginSettingsLink(QWidget *parent, QString text, QString description)
	: Ui::SettingsButton(parent, rpl::single(std::move(text)), st::settingsPluginLinkRow) {
		if (!description.isEmpty()) {
			_description = Ui::CreateChild<Ui::FlatLabel>(
				this, std::move(description), st::settingsPluginCardMeta);
			_description->setAttribute(Qt::WA_TransparentForMouseEvents);
			_description->show();
		}
		setAllowTextLines();
		AddRowIcon(this, st(), st::settingsPluginSettingsIcon);
	}

protected:
	int resizeGetHeight(int width) override {
		const auto height = Ui::SettingsButton::resizeGetHeight(width);
		if (!_description) {
			return height;
		}
		const auto &padding = st().padding;
		_description->resizeToWidth(std::max(1, width - padding.left() - padding.right()));
		_description->moveToLeft(padding.left(),
			height - padding.bottom() + st::settingsPluginLinkTextSkip, width);
		return height + st::settingsPluginLinkTextSkip + _description->height();
	}

	void paintEvent(QPaintEvent *event) override {
		{
			auto p = QPainter(this);
			const auto quality = PainterHighQualityEnabler(p);
			const auto over = (isOver() || isDown()) && !isDisabled();
			const auto radius = st::settingsPluginCardRadius;
			p.setPen(Qt::NoPen);
			p.setBrush(over ? st::settingsPluginRowPressed : st::settingsPluginRowHover);
			p.drawRoundedRect(rect(), radius, radius);
			const auto &arrow = st::settingsPluginLinkArrow;
			arrow.paint(p, width() - st::settingsPluginTextMargin.right() - arrow.width(),
				(height() - arrow.height()) / 2, width());
		}
		Ui::SettingsButton::paintEvent(event);
	}

private:
	Ui::FlatLabel *_description = nullptr;
};

} // namespace

void AddPluginManagement(
		not_null<Ui::VerticalLayout*> layout,
		const QJsonObject &plugin,
		Fn<void()> updateFile,
		Fn<void(QString)> operate,
		Fn<void()> share,
		Fn<void()> remove) {
	AddDetailsSection(layout, Tr(u"Plugin management"_q, u"Управление плагином"_q));
	AddMenuAction(layout, Tr(u"Update from file"_q, u"Обновить из файла"_q), st::settingsPluginFileIcon,
		std::move(updateFile));
	if (plugin.value(u"update"_q).isObject()) {
		const auto offer = plugin.value(u"update"_q).toObject();
		AddMenuAction(layout, offer.value(u"rollback"_q).toBool()
			? Tr(u"Install previous safe version"_q, u"Установить предыдущую безопасную версию"_q)
			: Tr(u"Update from registry"_q, u"Обновить из реестра"_q),
			st::settingsPluginRegistryIcon,
			[=] { operate(u"apply_update"_q); });
	}
	if (plugin.value(u"previous"_q).toBool()) {
		AddMenuAction(layout, Tr(u"Roll back"_q, u"Откатить"_q), st::settingsPluginRollbackIcon,
			[=] { operate(u"rollback"_q); });
	}
	AddMenuAction(layout, Tr(u"Share plugin"_q, u"Отправить плагин"_q), st::settingsPluginShareIcon,
		std::move(share));
	const auto divider = layout->add(
		object_ptr<Ui::FixedHeightWidget>(layout, st::lineWidth),
		style::margins(st::settingsPluginMenuRow.padding.left(), 0,
			st::settingsPluginMenuRow.padding.right(), 0));
	divider->paintRequest() | rpl::on_next([=] {
		auto color = st::windowFg->c;
		color.setAlphaF(st::settingsPluginDividerAlpha);
		QPainter(divider).fillRect(divider->rect(), color);
	}, divider->lifetime());
	AddMenuAction(layout, Tr(u"Remove plugin"_q, u"Удалить плагин"_q), st::settingsPluginRemoveIcon,
		std::move(remove), st::settingsPluginMenuRemove);
	Ui::AddSkip(layout);
}

void AddPluginSettings(
		not_null<Ui::VerticalLayout*> layout,
		const QJsonArray &fields,
		Fn<void(QJsonValue, bool)> activate,
		Fn<void(QJsonObject)> save) {
	AddDetailsSection(layout, Tr(u"Plugin settings"_q, u"Настройки плагина"_q));

	auto values = std::vector<std::pair<QString, Fn<QJsonValue()>>>();
	auto seenKeys = std::vector<QString>();
	const auto acceptKey = [&](const QString &key) {
		if (key.isEmpty() || std::find(begin(seenKeys), end(seenKeys), key) != end(seenKeys)) {
			return false;
		}
		seenKeys.push_back(key);
		return true;
	};
	for (const auto &value : fields) {
		const auto field = value.toObject();
		const auto kind = field.value(u"kind"_q).toString();
		const auto text = field.value(u"text"_q).toString();
		const auto initial = field.value(u"value"_q);
		const auto key = field.value(u"key"_q).toString();
		if (kind == u"header"_q) {
			AddPluginLabel(layout, text, st::settingsPluginSettingHint,
				st::settingsPluginSectionMargin);
		} else if (kind == u"button"_q || kind == u"link"_q) {
			const auto link = kind == u"link"_q;
			const auto actionId = field.value(u"action_id"_q);
			auto callback = [=] { activate(actionId, link); };
			if (link) {
				const auto row = layout->add(object_ptr<PluginSettingsLink>(
					layout, text, field.value(u"description"_q).toString()),
					st::settingsPluginLinkMargin, style::al_justify);
				row->setClickedCallback(std::move(callback));
			} else {
				const auto button = AddAction(layout, text, std::move(callback));
				button->setAllowTextLines();
			}
		} else if (kind == u"switch"_q) {
			if (!acceptKey(key)) continue;
			const auto button = layout->add(object_ptr<Ui::SettingsButton>(
				layout,
				rpl::single(text),
				st::settingsPluginSettingRow
			))->toggleOn(rpl::single(initial.toBool()));
			button->setAllowTextLines();
			values.emplace_back(key, [=] { return QJsonValue(button->toggled()); });
		} else if (kind == u"selector"_q) {
			if (!acceptKey(key)) continue;
			AddPluginLabel(layout, text, st::settingsPluginSettingHint,
				st::settingsPluginSectionMargin);
			const auto group = std::make_shared<Ui::RadiobuttonGroup>(initial.toInt());
			auto index = 0;
			for (const auto &item : field.value(u"items"_q).toArray()) {
				layout->add(
					object_ptr<Ui::Radiobutton>(
						layout,
						group,
						index++,
						item.toString(),
						st::settingsPluginSelector),
					st::settingsPluginSelectorMargin);
			}
			values.emplace_back(key, [=] { return QJsonValue(group->current()); });
		} else if (kind == u"input"_q) {
			if (!acceptKey(key)) continue;
			const auto side = st::settingsPluginTextMargin.left();
			const auto input = layout->add(
				object_ptr<Ui::InputField>(
					layout,
					st::defaultInputField,
					Ui::InputField::Mode::SingleLine,
					rpl::single(text),
					TextWithTags{ initial.toString() }),
				style::margins(side, style::ConvertScale(2), side, style::ConvertScale(8)));
			values.emplace_back(key, [=] { return QJsonValue(input->getLastText()); });
		}
	}
	if (values.empty()) {
		return;
	}
	AddAction(layout, Tr(u"Save settings"_q, u"Сохранить настройки"_q), [save = std::move(save), values = std::move(values)] {
		auto object = QJsonObject();
		for (const auto &[key, value] : values) {
			object.insert(key, value());
		}
		save(std::move(object));
	}, st::settingsPluginSave);
}

void BuildPluginRemovalBox(
		not_null<Ui::GenericBox*> box,
		rpl::producer<QString> cancel,
		Fn<void(bool)> remove) {
	box->setTitle(rpl::single(Tr(u"Remove Plugin"_q, u"Удаление плагина"_q)));
	box->addTopButton(st::settingsPluginClose, [=] { box->closeBox(); })
		->setAccessibleName(Tr(u"Close"_q, u"Закрыть"_q));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		Tr(u"Remove the selected plugin instance?"_q, u"Удалить выбранный экземпляр плагина?"_q),
		st::boxLabel));
	const auto keepData = box->addRow(
		object_ptr<Ui::Checkbox>(
			box,
			Tr(u"Keep settings and data"_q, u"Сохранить настройки и данные"_q),
			false,
			st::defaultBoxCheckbox),
		style::margins(
			st::boxRowPadding.left(),
			style::ConvertScale(12),
			st::boxRowPadding.right(),
			style::ConvertScale(4)));
	box->addLeftButton(std::move(cancel), [=] { box->closeBox(); }, st::settingsPluginDialog);
	box->addButton(rpl::single(Tr(u"Remove"_q, u"Удалить"_q)), [=] {
		const auto keep = keepData->checked();
		const auto invoke = remove;
		box->closeBox();
		invoke(keep);
	}, st::settingsPluginDialogDanger);
}

} // namespace Ovygram
