#include "ovygram/plugin_list_view.h"
#include "ovygram/i18n.h"
#include "ovygram/plugin_trust_badge.h"
#include "styles/style_layers.h"
#include "styles/style_plugin_management.h"
#include "ui/effects/animation_value.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"

namespace Ovygram {
namespace {

int Px(int value) {
	return style::ConvertScale(value);
}

style::margins Inset(int top = 0, int bottom = 0) {
	const auto side = st::settingsPluginTextMargin.left();
	return { side, Px(top), side, Px(bottom) };
}

QColor SecondaryText() {
	return anim::color(st::settingsPluginLabelMuted, st::windowFg, st::settingsPluginSecondaryTextBlend);
}

void UseSecondaryText(not_null<Ui::FlatLabel*> label) {
	const auto apply = [=] { label->setTextColorOverride(SecondaryText()); };
	apply();
	style::PaletteChanged() | rpl::on_next(apply, label->lifetime());
}

int RowInset() {
	return Px(12);
}

int ArrowSpace() {
	return st::settingsPluginLinkArrow.width() + Px(16);
}

Ui::SettingsButton *Action(
		not_null<Ui::VerticalLayout*> layout,
		QString text,
		Fn<void()> callback,
		const style::SettingsButton &style = st::settingsPluginPlainRow) {
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
		? Tr(u"Safe version %1 available"_q, u"Доступна безопасная версия %1"_q).arg(version)
		: Tr(u"New version %1"_q, u"Новая версия %1"_q).arg(version);
}

QString TrustLabel(const QString &status) {
	if (status == u"official"_q) return Tr(u"Official"_q, u"Официальный"_q);
	if (status == u"verified"_q) return Tr(u"Verified"_q, u"Проверен"_q);
	if (status == u"blocked"_q) return Tr(u"Blocked"_q, u"Заблокирован"_q);
	if (status == u"revoked"_q) return Tr(u"Verification revoked"_q, u"Проверка отозвана"_q);
	if (status == u"verification_unavailable"_q) return Tr(u"Verification unavailable"_q, u"Проверка недоступна"_q);
	return Tr(u"Unverified"_q, u"Не проверен"_q);
}

class Actions final : public Ui::RpWidget {
public:
	Actions(QWidget *parent, Fn<void()> import, Fn<void()> check)
	: Ui::RpWidget(parent)
	, _import(Ui::CreateChild<Ui::RoundButton>(this, rpl::single(Tr(u"Import plugin"_q, u"Импортировать плагин"_q)),
		st::settingsPluginInstallButton))
	, _check(Ui::CreateChild<Ui::RoundButton>(this, rpl::single(Tr(u"Check for updates"_q, u"Проверить обновления"_q)),
		st::settingsPluginInstallSecondary)) {
		_import->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		_check->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		_import->setClickedCallback(std::move(import));
		_check->setClickedCallback(std::move(check));
	}

	not_null<Ui::RoundButton*> checkButton() const { return _check; }

protected:
	int resizeGetHeight(int width) override {
		const auto gap = Px(8);
		const auto checkLeft = _import->width() + gap;
		const auto fits = checkLeft + _check->width() <= width;
		const auto checkTop = fits ? 0 : _import->height() + gap;
		_import->moveToLeft(0, 0, width);
		_check->moveToLeft(fits ? checkLeft : 0, checkTop, width);
		return checkTop + _check->height();
	}

private:
	const not_null<Ui::RoundButton*> _import;
	const not_null<Ui::RoundButton*> _check;
};

class ListEntry final : public Ui::SettingsButton {
public:
	ListEntry(QWidget *parent, const QJsonObject &plugin)
	: Ui::SettingsButton(parent, rpl::single(I18n::PluginName(plugin)), st::settingsPluginCardTitle)
	, _loaded(plugin.value(u"loaded"_q).toBool())
	, _blocked(plugin.value(u"trust"_q).toObject().value(u"status"_q).toString() == u"blocked"_q)
	, _meta(Ui::CreateChild<Ui::FlatLabel>(this, summary(plugin), st::settingsPluginCardMeta)) {
		const auto trust = plugin.value(u"trust"_q).toObject().value(u"status"_q).toString();
		_trustBadge = MakePluginBadge(this, TrustLabel(trust), trust).release();
		_trustBadge->setAttribute(Qt::WA_TransparentForMouseEvents);
		_trustBadge->show();
		const auto update = UpdateLabel(plugin);
		if (!update.isEmpty()) {
			_update = Ui::CreateChild<Ui::FlatLabel>(this, update, st::settingsPluginCardMeta);
			_update->setTextColorOverride(st::settingsPluginLabelAccent->c);
			_update->setAttribute(Qt::WA_TransparentForMouseEvents);
		}
		const auto error = plugin.value(u"error"_q).toString().simplified();
		if (!error.isEmpty()) {
			constexpr auto kLimit = 160;
			const auto excerpt = (error.size() > kLimit)
				? error.left(kLimit).trimmed() + u"…"_q : error;
			_error = Ui::CreateChild<Ui::FlatLabel>(this,
				PluginErrorTitle(plugin) + excerpt, st::settingsPluginCardMeta);
			_error->setTextColorOverride(SecondaryText());
			_error->setAttribute(Qt::WA_TransparentForMouseEvents);
		}
		const auto applyColors = [this] {
			_meta->setTextColorOverride(SecondaryText());
			if (_update) _update->setTextColorOverride(st::settingsPluginLabelAccent->c);
			if (_error) _error->setTextColorOverride(SecondaryText());
			this->update();
		};
		_meta->setAttribute(Qt::WA_TransparentForMouseEvents);
		applyColors();
		style::PaletteChanged() | rpl::on_next(applyColors, lifetime());
		setPaddingOverride({ RowInset(), Px(12), ArrowSpace(), 0 });
		setAllowTextLines();
	}

	void setActionSpace(int width) {
		if (_actionSpace != width) {
			_actionSpace = width;
			setPaddingOverride({ RowInset(), Px(12), ArrowSpace() + width, 0 });
		}
	}

protected:
	int resizeGetHeight(int width) override {
		const auto titleHeight = Ui::SettingsButton::resizeGetHeight(width);
		const auto left = RowInset();
		const auto textWidth = std::max(1, width - left - ArrowSpace() - _actionSpace);
		const auto dotSpace = Px(13);
		const auto badgeWidth = std::min(textWidth, _trustBadge->naturalWidth());
		const auto gap = Px(8);
		const auto badgeLeft = dotSpace + _meta->naturalWidth() + gap;
		const auto inlineBadge = badgeLeft + badgeWidth <= textWidth;

		_meta->resizeToWidth(std::max(1, inlineBadge ? _meta->naturalWidth() : textWidth - dotSpace));
		_trustBadge->resizeToWidth(badgeWidth);
		const auto metaHeight = _meta->height();
		const auto badgeHeight = _trustBadge->height();
		const auto metaTop = titleHeight + (inlineBadge ? std::max(0, badgeHeight - metaHeight) / 2 : 0);
		_meta->moveToLeft(left + dotSpace, metaTop, width);
		const auto badgeTop = inlineBadge
			? titleHeight + std::max(0, metaHeight - badgeHeight) / 2
			: metaTop + metaHeight + Px(4);
		_trustBadge->moveToLeft(left + (inlineBadge ? badgeLeft : 0), badgeTop, width);
		auto bottom = std::max(metaTop + metaHeight, badgeTop + badgeHeight);
		for (const auto label : { _update, _error }) {
			if (label) {
				label->resizeToWidth(textWidth);
				label->moveToLeft(left, bottom + Px(5), width);
				bottom += Px(5) + label->height();
			}
		}
		return bottom + Px(12);
	}

	void onStateChanged(State was, StateChangeSource source) override {
		Ui::SettingsButton::onStateChanged(was, source);
		parentWidget()->update();
	}

	void paintEvent(QPaintEvent *event) override {
		Ui::SettingsButton::paintEvent(event);
		auto p = QPainter(this);
		const auto quality = PainterHighQualityEnabler(p);
		const auto dot = Px(7);
		p.setPen(Qt::NoPen);
		p.setBrush(_blocked
			? PluginTrustColor(u"blocked"_q)
			: _loaded ? PluginTrustColor(u"verified"_q) : st::windowSubTextFg->c);
		p.drawEllipse(style::rtlrect(RowInset(), _meta->y() + Px(4), dot, dot, width()));
		const auto &arrow = st::settingsPluginLinkArrow;
		arrow.paint(p, width() - arrow.width() - Px(8), (height() - arrow.height()) / 2, width());
	}

private:
	QString summary(const QJsonObject &plugin) const {
		const auto status = _blocked ? Tr(u"Blocked"_q, u"Запуск запрещён"_q)
			: plugin.value(u"installing"_q).toBool() ? Tr(u"Installing dependencies…"_q, u"Установка зависимостей…"_q)
			: _loaded ? Tr(u"Running"_q, u"Запущен"_q)
			: plugin.value(u"enabled"_q).toBool() ? Tr(u"Enabled, not running"_q, u"Включён, но не запущен"_q)
			: Tr(u"Stopped"_q, u"Остановлен"_q);
		auto result = status + u" · v"_q + plugin.value(u"version"_q).toString();
		if (plugin.value(u"instances"_q).toInt() > 1) {
			result += u" · "_q + plugin.value(u"installation_id"_q).toString().left(8);
		}
		return result;
	}

	const bool _loaded;
	const bool _blocked;
	const not_null<Ui::FlatLabel*> _meta;
	Ui::VerticalLayout *_trustBadge = nullptr;
	Ui::FlatLabel *_update = nullptr;
	Ui::FlatLabel *_error = nullptr;
	int _actionSpace = 0;
};

class ListToggle final : public Ui::SettingsButton {
public:
	explicit ListToggle(QWidget *parent)
	: Ui::SettingsButton(parent, nullptr, st::settingsPluginToggleAction) {
	}

	QString accessibilityName() override {
		return accessibleName();
	}
};

class ListUpdateButton final : public Ui::RoundButton {
public:
	ListUpdateButton(QWidget *parent, QString title, const QString &name)
	: Ui::RoundButton(parent, rpl::single(title), st::settingsPluginListUpdate) {
		setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		setAccessibleName(title + u" · "_q + name);
	}

	QString accessibilityName() override {
		return accessibleName();
	}
};

class ListRow final : public Ui::RpWidget {
public:
	ListRow(
			QWidget *parent,
			const QJsonObject &plugin,
			bool launchBlocked,
			Fn<void()> open,
			Fn<void()> toggle,
			Fn<void()> applyUpdate)
	: Ui::RpWidget(parent)
	, _open(Ui::CreateChild<ListEntry>(this, plugin))
	, _toggle(Ui::CreateChild<ListToggle>(this)) {
		const auto enabled = plugin.value(u"enabled"_q).toBool();
		const auto blocked = plugin.value(u"trust"_q).toObject().value(u"status"_q).toString() == u"blocked"_q;
		const auto displayName = I18n::PluginName(plugin);
		_open->setClickedCallback(std::move(open));
		_toggle->toggleOn(rpl::single(enabled), true);
		_toggle->setAccessibleName((enabled
			? Tr(u"Disable %1"_q, u"Отключить %1"_q)
			: Tr(u"Enable %1"_q, u"Включить %1"_q)).arg(displayName));
		_toggle->setClickedCallback(std::move(toggle));
		_toggle->setDisabled((launchBlocked || blocked) && !enabled);
		const auto offer = plugin.value(u"update"_q).toObject();
		const auto updating = plugin.value(u"updating"_q).toBool();
		if (!offer.isEmpty() || updating) {
			const auto title = updating ? Tr(u"Updating…"_q, u"Обновляется…"_q)
				: offer.value(u"rollback"_q).toBool()
				? Tr(u"Roll back"_q, u"Откатить"_q)
				: Tr(u"Update"_q, u"Обновить"_q);
			_applyUpdate = Ui::CreateChild<ListUpdateButton>(this, title, displayName);
			_applyUpdate->setClickedCallback(std::move(applyUpdate));
			_applyUpdate->setDisabled(updating);
			if (updating) {
				const auto apply = [button = _applyUpdate] { button->setTextFgOverride(SecondaryText()); };
				apply();
				style::PaletteChanged() | rpl::on_next(apply, _applyUpdate->lifetime());
			}
		}
	}

protected:
	int resizeGetHeight(int width) override {
		const auto toggleWidth = std::min(width, st::settingsPluginToggleWidth);
		const auto openWidth = width - toggleWidth;
		const auto actionSpace = _applyUpdate ? _applyUpdate->width() + Px(12) : 0;
		const auto inlineAction = openWidth - RowInset() - ArrowSpace() - actionSpace >= Px(245);
		_open->setActionSpace(inlineAction ? actionSpace : 0);
		_open->resizeToWidth(openWidth);
		_open->moveToLeft(0, 0, width);
		_toggle->resizeToWidth(toggleWidth);
		_toggle->moveToRight(0, (_open->height() - _toggle->height()) / 2, width);
		auto bottom = _open->height();
		if (_applyUpdate) {
			if (inlineAction) {
				_applyUpdate->moveToRight(toggleWidth + ArrowSpace(),
					(bottom - _applyUpdate->height()) / 2, width);
			} else {
				_applyUpdate->moveToLeft(RowInset(), bottom - Px(2), width);
				bottom += _applyUpdate->height() + Px(10);
			}
		}
		return bottom + Px(5) + st::lineWidth;
	}

	void paintEvent(QPaintEvent *) override {
		auto p = QPainter(this);
		if (_open->isOver() || _open->isDown()) {
			const auto quality = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(_open->isDown() ? st::settingsPluginRowPressed : st::settingsPluginRowHover);
			p.drawRoundedRect(rect().adjusted(0, 0, 0, -Px(5) - st::lineWidth),
				st::settingsPluginRowRadius, st::settingsPluginRowRadius);
		}
		auto color = st::windowFg->c;
		color.setAlphaF(st::settingsPluginDividerAlpha);
		p.fillRect(RowInset(), height() - st::lineWidth, width() - 2 * RowInset(), st::lineWidth, color);
	}

private:
	const not_null<ListEntry*> _open;
	const not_null<Ui::SettingsButton*> _toggle;
	Ui::RoundButton *_applyUpdate = nullptr;
};

} // namespace

QString PluginErrorTitle(const QJsonObject &plugin) {
	// Attribute messages from the plugin process to the plugin.
	return plugin.value(u"error_from_plugin"_q).toBool()
		? Tr(u"Plugin message: "_q, u"Сообщение плагина: "_q)
		: Tr(u"Error: "_q, u"Ошибка: "_q);
}

not_null<Ui::InputField*> AddPluginListSearch(not_null<Ui::VerticalLayout*> layout) {
	const auto row = layout->add(object_ptr<Ui::FixedHeightWidget>(layout, Px(38)),
		Inset(14, 12));
	const auto placeholder = Tr(u"Search plugins"_q, u"Поиск плагинов"_q);
	const auto field = Ui::CreateChild<Ui::InputField>(row, st::defaultMultiSelectSearchField,
		rpl::single(placeholder));
	field->setAccessibleName(placeholder);
	const auto clear = Ui::CreateChild<Ui::CrossButton>(row, st::settingsPluginSearchClear);
	clear->setAccessibleName(Tr(u"Clear search"_q, u"Очистить поиск"_q));
	clear->setClickedCallback([=] { field->setText({}); });
	clear->toggle(false, anim::type::instant);
	field->changes() | rpl::on_next([=] {
		clear->toggle(!field->getLastText().isEmpty(), anim::type::instant);
	}, clear->lifetime());
	row->sizeValue() | rpl::on_next([=](QSize size) {
		const auto left = Px(36);
		field->resizeToWidth(std::max(1, size.width() - left - clear->width()));
		field->moveToLeft(left, (size.height() - field->height()) / 2, size.width());
		clear->moveToRight(0, (size.height() - clear->height()) / 2, size.width());
	}, row->lifetime());
	row->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(row);
		const auto quality = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::settingsPluginCardFill);
		p.drawRoundedRect(row->rect(), Px(8), Px(8));
		st::boxFieldSearchIcon.paint(p, 0, Px(2), row->width());
	}, row->lifetime());
	return field;
}

void AddPluginListNotice(
		not_null<Ui::VerticalLayout*> layout,
		rpl::producer<QString> text) {
	const auto wrap = layout->add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		layout, object_ptr<Ui::VerticalLayout>(layout)), style::al_justify);
	const auto card = wrap->entity()->add(object_ptr<Ui::VerticalLayout>(wrap->entity()),
		Inset(0, 12), style::al_justify);
	const auto label = card->add(object_ptr<Ui::FlatLabel>(card, rpl::duplicate(text),
		st::settingsPluginCardMeta), { RowInset(), Px(9), RowInset(), Px(9) }, style::al_justify);
	UseSecondaryText(label);
	card->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(card);
		const auto quality = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		p.setBrush(st::settingsPluginCardFill);
		p.drawRoundedRect(card->rect(), st::settingsPluginActionRadius, st::settingsPluginActionRadius);
	}, card->lifetime());
	wrap->toggleOn(std::move(text) | rpl::map([](const QString &value) {
		return !value.isEmpty();
	}), anim::type::instant);
}

not_null<Ui::RoundButton*> AddPluginListActions(
		not_null<Ui::VerticalLayout*> layout,
		Fn<void()> import,
		Fn<void()> check) {
	return layout->add(object_ptr<Actions>(
		layout, std::move(import), std::move(check)), Inset(0, 14))->checkButton();
}

void AddPluginListRows(
		not_null<Ui::VerticalLayout*> layout,
		const QJsonArray &plugins,
		const QString &query,
		bool launchBlocked,
		Fn<void(QJsonObject)> open,
		Fn<void(QString id, bool enabled)> setEnabled,
		Fn<void(QString id)> applyUpdate,
		Fn<void()> log) {
	const auto textInset = Inset().left() + RowInset();
	layout->add(object_ptr<Ui::FlatLabel>(layout, Tr(u"Installed plugins"_q, u"Установленные плагины"_q),
		st::settingsPluginSectionTitle), { textInset, Px(4), textInset, Px(6) });
	auto found = false;
	for (const auto &entry : plugins) {
		const auto plugin = entry.toObject();
		const auto name = plugin.value(u"name"_q).toString();
		const auto localizedName = I18n::PluginName(plugin);
		if (!name.contains(query, Qt::CaseInsensitive)
			&& !localizedName.contains(query, Qt::CaseInsensitive)
			&& !plugin.value(u"plugin_id"_q).toString().contains(query, Qt::CaseInsensitive)
			&& !plugin.value(u"author"_q).toString().contains(query, Qt::CaseInsensitive)) {
			continue;
		}
		found = true;
		const auto id = plugin.value(u"installation_id"_q).toString();
		const auto enabled = plugin.value(u"enabled"_q).toBool();
		layout->add(object_ptr<ListRow>(layout, plugin, launchBlocked,
			[=] { open(plugin); },
			[=] { setEnabled(id, !enabled); },
			[=] { applyUpdate(id); }), Inset(3, 0));
	}
	if (!found) {
		UseSecondaryText(layout->add(object_ptr<Ui::FlatLabel>(layout,
			plugins.isEmpty()
				? Tr(u"No plugins installed yet."_q, u"Установленных плагинов пока нет."_q)
				: Tr(u"No plugins found."_q, u"Плагины не найдены."_q),
			st::settingsPluginCardMeta), { textInset, Px(8), textInset, Px(12) }));
	}
	const auto journal = Action(layout, Tr(u"Open log"_q, u"Открыть журнал"_q), std::move(log));
	journal->setPaddingOverride({ textInset, Px(12), textInset, Px(8) });
	journal->setColorOverride(SecondaryText());
	style::PaletteChanged() | rpl::on_next([journal] {
		journal->setColorOverride(SecondaryText());
	}, journal->lifetime());
	Ui::AddSkip(layout);
}

} // namespace Ovygram
