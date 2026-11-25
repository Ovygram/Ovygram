#include "ovygram/plugin_list_view.h"
#include "ovygram/plugin_trust_badge.h"
#include "styles/style_layers.h"
#include "styles/style_plugin_management.h"
#include "ui/effects/animation_value.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
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
		? u"Доступна безопасная версия %1"_q.arg(version)
		: u"Доступно обновление до %1"_q.arg(version);
}

QString TrustLabel(const QString &status) {
	if (status == u"official"_q) return u"Официальный"_q;
	if (status == u"verified"_q) return u"Проверен"_q;
	if (status == u"blocked"_q) return u"Заблокирован"_q;
	if (status == u"revoked"_q) return u"Проверка отозвана"_q;
	if (status == u"verification_unavailable"_q) return u"Проверка недоступна"_q;
	return u"Не проверен"_q;
}

class Actions final : public Ui::RpWidget {
public:
	Actions(QWidget *parent, Fn<void()> import, Fn<void()> check)
	: Ui::RpWidget(parent)
	, _import(Ui::CreateChild<Ui::RoundButton>(this, rpl::single(u"Импортировать плагин"_q),
		st::settingsPluginInstallButton))
	, _check(Ui::CreateChild<Ui::RoundButton>(this, rpl::single(u"Проверить обновления"_q),
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
	: Ui::SettingsButton(parent, rpl::single(plugin.value(u"name"_q).toString()), st::settingsPluginCardTitle)
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
		_meta->setAttribute(Qt::WA_TransparentForMouseEvents);
		_meta->setTextColorOverride(SecondaryText());
		setPaddingOverride({ 0, Px(10), Px(27), 0 });
		setAllowTextLines();
	}

protected:
	int resizeGetHeight(int width) override {
		const auto titleHeight = Ui::SettingsButton::resizeGetHeight(width);
		const auto textWidth = std::max(1, width - Px(27));
		const auto metaLeft = Px(13);
		const auto badgeWidth = std::min(textWidth, _trustBadge->naturalWidth());
		const auto gap = Px(8);
		const auto badgeLeft = metaLeft + _meta->naturalWidth() + gap;
		const auto inlineBadge = badgeLeft + badgeWidth <= textWidth;

		_meta->resizeToWidth(std::max(1, inlineBadge ? _meta->naturalWidth() : textWidth - metaLeft));
		_trustBadge->resizeToWidth(badgeWidth);
		const auto metaHeight = _meta->height();
		const auto badgeHeight = _trustBadge->height();
		const auto metaTop = titleHeight + (inlineBadge ? std::max(0, badgeHeight - metaHeight) / 2 : 0);
		_meta->moveToLeft(metaLeft, metaTop, width);
		const auto badgeTop = inlineBadge
			? titleHeight + std::max(0, metaHeight - badgeHeight) / 2
			: metaTop + metaHeight + Px(4);
		_trustBadge->moveToLeft(inlineBadge ? badgeLeft : 0, badgeTop, width);
		auto bottom = std::max(metaTop + metaHeight, badgeTop + badgeHeight);
		for (const auto label : { _update, _error }) {
			if (label) {
				label->resizeToWidth(textWidth);
				label->moveToLeft(0, bottom + Px(3), width);
				bottom += Px(3) + label->height();
			}
		}
		return bottom + Px(10);
	}

	void paintEvent(QPaintEvent *event) override {
		if (isOver() || isDown()) {
			auto p = QPainter(this);
			const auto quality = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(isDown() ? st::settingsPluginRowPressed : st::settingsPluginRowHover);
			p.drawRoundedRect(rect(), st::settingsPluginRowRadius, st::settingsPluginRowRadius);
		}
		Ui::SettingsButton::paintEvent(event);
		auto p = QPainter(this);
		const auto quality = PainterHighQualityEnabler(p);
		const auto dot = Px(7);
		p.setPen(Qt::NoPen);
		p.setBrush(_blocked
			? PluginTrustColor(u"blocked"_q)
			: _loaded ? PluginTrustColor(u"verified"_q) : st::windowSubTextFg->c);
		p.drawEllipse(style::rtlrect(0, _meta->y() + Px(4), dot, dot, width()));
		const auto &arrow = st::settingsPluginLinkArrow;
		arrow.paint(p, width() - arrow.width() - Px(8), (height() - arrow.height()) / 2, width());
	}

private:
	QString summary(const QJsonObject &plugin) const {
		const auto status = _blocked ? u"Запуск запрещён"_q
			: plugin.value(u"installing"_q).toBool() ? u"Установка зависимостей…"_q
			: _loaded ? u"Запущен"_q
			: plugin.value(u"enabled"_q).toBool() ? u"Включён, но не запущен"_q
			: u"Остановлен"_q;
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

class ListRow final : public Ui::RpWidget {
public:
	ListRow(
			QWidget *parent,
			const QJsonObject &plugin,
			bool launchBlocked,
			Fn<void()> open,
			Fn<void()> toggle)
	: Ui::RpWidget(parent)
	, _open(Ui::CreateChild<ListEntry>(this, plugin))
	, _toggle(Ui::CreateChild<ListToggle>(this)) {
		const auto enabled = plugin.value(u"enabled"_q).toBool();
		const auto blocked = plugin.value(u"trust"_q).toObject().value(u"status"_q).toString() == u"blocked"_q;
		_open->setClickedCallback(std::move(open));
		_toggle->toggleOn(rpl::single(enabled), true);
		_toggle->setAccessibleName((enabled ? u"Отключить %1"_q : u"Включить %1"_q)
			.arg(plugin.value(u"name"_q).toString()));
		_toggle->setClickedCallback(std::move(toggle));
		_toggle->setDisabled((launchBlocked || blocked) && !enabled);
	}

protected:
	int resizeGetHeight(int width) override {
		const auto toggleWidth = std::min(width, st::settingsPluginToggleWidth);
		_open->resizeToWidth(width - toggleWidth);
		_open->moveToLeft(0, 0, width);
		_toggle->resizeToWidth(toggleWidth);
		_toggle->moveToRight(0, (_open->height() - _toggle->height()) / 2, width);
		return _open->height() + st::lineWidth;
	}

	void paintEvent(QPaintEvent *) override {
		auto color = st::windowFg->c;
		color.setAlphaF(st::settingsPluginDividerAlpha);
		QPainter(this).fillRect(0, height() - st::lineWidth, width(), st::lineWidth, color);
	}

private:
	const not_null<ListEntry*> _open;
	const not_null<Ui::SettingsButton*> _toggle;
};

} // namespace

QString PluginErrorTitle(const QJsonObject &plugin) {
	// Attribute messages from the plugin process to the plugin.
	return plugin.value(u"error_from_plugin"_q).toBool()
		? u"Сообщение плагина: "_q
		: u"Ошибка: "_q;
}

not_null<Ui::InputField*> AddPluginListSearch(not_null<Ui::VerticalLayout*> layout) {
	const auto row = layout->add(object_ptr<Ui::FixedHeightWidget>(layout, Px(38)),
		Inset(12, 0));
	const auto field = Ui::CreateChild<Ui::InputField>(row, st::defaultMultiSelectSearchField,
		rpl::single(u"Поиск плагинов"_q));
	field->setAccessibleName(u"Поиск плагинов"_q);
	const auto clear = Ui::CreateChild<Ui::CrossButton>(row, st::settingsPluginSearchClear);
	clear->setAccessibleName(u"Очистить поиск"_q);
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

not_null<Ui::RoundButton*> AddPluginListActions(
		not_null<Ui::VerticalLayout*> layout,
		Fn<void()> import,
		Fn<void()> check) {
	return layout->add(object_ptr<Actions>(
		layout, std::move(import), std::move(check)), Inset(12, 12))->checkButton();
}

void AddPluginListRows(
		not_null<Ui::VerticalLayout*> layout,
		const QJsonArray &plugins,
		const QString &query,
		bool launchBlocked,
		Fn<void(QJsonObject)> open,
		Fn<void(QString id, bool enabled)> setEnabled,
		Fn<void()> log) {
	layout->add(object_ptr<Ui::FlatLabel>(layout, u"Установленные плагины"_q,
		st::settingsPluginSectionTitle), Inset(7, 5));
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
		layout->add(object_ptr<ListRow>(layout, plugin, launchBlocked,
			[=] { open(plugin); },
			[=] { setEnabled(id, !enabled); }), Inset());
	}
	if (!found) {
		Ui::AddDividerText(layout, rpl::single(plugins.isEmpty()
			? u"Установленных плагинов пока нет."_q : u"Плагины не найдены."_q),
			Inset(8, 12));
	}
	const auto journal = Action(layout, u"Открыть журнал"_q, std::move(log));
	journal->setPaddingOverride(Inset(10, 8));
	journal->setColorOverride(SecondaryText());
	Ui::AddSkip(layout);
}

} // namespace Ovygram
