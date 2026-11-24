#include "ovygram/plugin_install_view.h"

#include "ovygram/plugin_management_view.h"
#include "ovygram/plugin_trust_badge.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/slide_wrap.h"
#include "styles/style_layers.h"

#include <QtCore/QJsonArray>

#include <algorithm>

namespace Ovygram {
namespace {

QString StatusText(const QString &status) {
	if (status == u"official"_q) return u"Официальный плагин"_q;
	if (status == u"verified"_q) return u"Проверенный плагин"_q;
	if (status == u"blocked"_q) return u"Опасный плагин — установка запрещена"_q;
	if (status == u"revoked"_q) return u"Проверка отозвана"_q;
	if (status == u"verification_unavailable"_q) return u"Проверка недоступна"_q;
	return u"Не проверен"_q;
}

QString Version(const QJsonObject &metadata) {
	return u"v%1"_q.arg(metadata.value(u"version"_q).toString());
}

const style::FlatLabel &TechnicalTextStyle() {
	static auto style = [] {
		auto result = st::settingsPluginInstallBadge;
		result.style.font = result.style.font->monospace();
		return result;
	}();
	return style;
}

class InstallLine final : public Ui::RpWidget {
public:
	explicit InstallLine(QWidget *parent, bool compact = false)
	: Ui::RpWidget(parent)
	, text(Ui::CreateChild<Ui::VerticalLayout>(this))
	, _compact(compact) {
		text->show();
	}

	void setTrailing(object_ptr<Ui::RpWidget> trailing) {
		_trailing = trailing.release();
		_trailing->show();
	}

	const not_null<Ui::VerticalLayout*> text;

protected:
	int resizeGetHeight(int width) override {
		const auto natural = _trailing ? _trailing->naturalWidth() : 0;
		const auto trailingWidth = std::min(width / 2, natural < 0 ? _trailing->width() : natural);
		const auto gap = !trailingWidth ? 0 : _compact
			? st::settingsPluginBadgeGap
			: st::settingsPluginInstallRowGap;
		const auto textWidth = std::max(width - trailingWidth - gap, 1);
		if (_compact) {
			text->resizeToNaturalWidth(textWidth);
		} else {
			text->resizeToWidth(textWidth);
		}
		if (!_trailing) {
			return text->height();
		}
		_trailing->resizeToWidth(trailingWidth);
		const auto height = std::max(text->height(), _trailing->height());
		text->moveToLeft(0, (height - text->height()) / 2, width);
		const auto trailingX = _compact ? text->width() + gap : width - trailingWidth;
		_trailing->moveToLeft(trailingX, (height - _trailing->height()) / 2, width);
		return height;
	}

private:
	const bool _compact;
	Ui::RpWidget *_trailing = nullptr;
};

class FileDetailsButton final : public Ui::SettingsButton {
public:
	explicit FileDetailsButton(QWidget *parent)
	: Ui::SettingsButton(parent, rpl::single(u"Подробности файла"_q), st::settingsPluginInstallDetails) {
	}

protected:
	void paintEvent(QPaintEvent *) override {
		auto painter = Painter(this);
		const auto over = (isOver() || isDown()) && !isDisabled();
		if (over) {
			const auto quality = PainterHighQualityEnabler(painter);
			const auto radius = st::settingsPluginCardRadius;
			painter.setPen(Qt::NoPen);
			painter.setBrush(st::windowBgRipple);
			painter.drawRoundedRect(rect().adjusted(0, -radius, 0, 0), radius, radius);
		}
		paintRipple(painter, 0, 0);
		paintText(painter, over, width());
		const auto &icon = st::settingsPluginInstallDetailsIcon;
		const auto &arrow = st::settingsPluginLinkArrow;
		icon.paint(painter, st().iconLeft, (height() - icon.height()) / 2, width());
		arrow.paint(painter, width() - st::settingsPluginTextMargin.right() - arrow.width(),
			(height() - arrow.height()) / 2, width());
	}
};

void AddSeparator(not_null<Ui::RpWidget*> widget) {
	widget->paintRequest() | rpl::on_next([=] {
		auto color = st::windowFg->c;
		color.setAlphaF(0.08);
		QPainter(widget).fillRect(widget->rect(), color);
	}, widget->lifetime());
}

object_ptr<Ui::VerticalLayout> VersionBadge(QWidget *parent, const QString &version) {
	auto badge = object_ptr<Ui::VerticalLayout>(parent);
	badge->add(object_ptr<Ui::FlatLabel>(badge, version, TechnicalTextStyle()),
		st::settingsPluginInstallBadgePadding)->setTextColorOverride(st::windowActiveTextFg->c);
	const auto widget = badge.data();
	widget->paintRequest() | rpl::on_next([=] {
		auto painter = QPainter(widget);
		const auto quality = PainterHighQualityEnabler(painter);
		auto fill = st::activeButtonBg->c;
		fill.setAlphaF(0.18);
		painter.setPen(Qt::NoPen);
		painter.setBrush(fill);
		const auto radius = st::settingsPluginInstallBadgeRadius;
		painter.drawRoundedRect(widget->rect(), radius, radius);
	}, widget->lifetime());
	return badge;
}

void AddInstalledRow(
		not_null<Ui::VerticalLayout*> rows,
		const QJsonObject &old,
		const QString &targetVersion,
		const style::RoundButton &buttonStyle,
		Fn<void()> update) {
	const auto card = AddPluginCard(rows);
	auto line = object_ptr<InstallLine>(card);
	const auto status = old.value(u"trust"_q).toObject().value(u"status"_q).toString();
	const auto id = old.value(u"installation_id"_q).toString().left(8);
	const auto previous = Version(old);
	auto delta = object_ptr<InstallLine>(line->text, true);
	const auto current = delta->text->add(object_ptr<Ui::FlatLabel>(
		delta->text, u"%1   %2 →"_q.arg(id, previous), TechnicalTextStyle()));
	current->setSelectable(true);
	auto target = object_ptr<Ui::FlatLabel>(delta, targetVersion, TechnicalTextStyle());
	target->setTextColorOverride(PluginTrustColor(u"verified"_q));
	delta->setTrailing(std::move(target));
	line->text->add(std::move(delta), style::al_justify);
	line->text->add(object_ptr<Ui::FlatLabel>(line->text,
		u"Текущая версия · %1"_q.arg(StatusText(status)), st::settingsPluginInstallBadge),
		style::al_justify);
	if (update) {
		auto button = object_ptr<Ui::RoundButton>(
			line, rpl::single(u"Обновить"_q), buttonStyle);
		button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		button->setClickedCallback([update = std::move(update)] {
			const auto invoke = update;
			invoke();
		});
		line->setTrailing(std::move(button));
	}
	card->add(std::move(line), st::settingsPluginInstallRowMargin, style::al_justify);
}

} // namespace

bool IsTrustedPlugin(const QString &status) {
	return status == u"verified"_q || status == u"official"_q;
}

not_null<Ui::VerticalLayout*> PreparePluginInstallBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(rpl::single(u"Установка плагина"_q));
	const auto outer = box->getDelegate()->outerContainer();
	const auto available = outer ? outer->width() - style::ConvertScale(32) : 0;
	box->setWidth(available > 0
		? std::min(available, st::settingsPluginsLayerWidth)
		: st::settingsPluginsLayerWidth);
	box->setMaxHeight(style::ConvertScale(680));
	AddSeparator(box->setPinnedToBottomContent(
		object_ptr<Ui::FixedHeightWidget>(box, st::lineWidth)));
	return box->addRow(object_ptr<Ui::VerticalLayout>(box), style::margins(), style::al_justify);
}

Ui::RoundButton *SetPluginInstallAction(
		not_null<Ui::GenericBox*> box,
		QString text,
		Fn<void()> callback,
		bool secondary) {
	box->clearButtons();
	box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); })->setAccessibleName(u"Закрыть"_q);
	if (!text.isEmpty()) {
		const auto button = box->addButton(rpl::single(std::move(text)),
			[callback = std::move(callback)] {
				const auto invoke = callback;
				invoke();
			},
			secondary ? st::settingsPluginInstallSecondary : st::settingsPluginInstallButton);
		button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		if (secondary) {
			auto border = st::windowFg->c;
			border.setAlphaF(0.16);
			button->setBrushOverride(st::windowBgOver->b);
			button->setPenOverride(QPen(border, st::lineWidth));
			button->setRippleOverride(st::lightButtonBgRipple->c);
		}
	}
	const auto cancel = box->addLeftButton(rpl::single(u"Отмена"_q),
		[=] { box->closeBox(); }, st::defaultLightButton);
	cancel->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
	return cancel.data();
}

void AddPluginInstallLabel(
		not_null<Ui::VerticalLayout*> rows,
		QString text,
		const style::FlatLabel &style) {
	auto label = object_ptr<Ui::FlatLabel>(rows, std::move(text), style);
	label->setSelectable(true);
	rows->add(std::move(label), st::settingsPluginTextMargin, style::al_justify);
}

not_null<Ui::VerticalLayout*> AddPluginInstallCard(
		not_null<Ui::VerticalLayout*> rows,
		const QJsonObject &metadata,
		const QJsonObject &trust) {
	const auto card = AddPluginCard(rows);
	auto title = object_ptr<InstallLine>(card);
	auto name = object_ptr<Ui::FlatLabel>(title->text,
		metadata.value(u"name"_q).toString(), st::settingsPluginInstallTitle);
	title->text->add(std::move(name), style::al_justify);
	title->setTrailing(VersionBadge(title, Version(metadata)));
	card->add(std::move(title), st::settingsPluginInstallRowMargin, style::al_justify);

	AddPluginInstallLabel(card, u"%1 · %2 · %3"_q.arg(
		metadata.value(u"display_author"_q).toString(metadata.value(u"author"_q).toString()),
		metadata.value(u"id"_q).toString(metadata.value(u"plugin_id"_q).toString()),
		metadata.value(u"release"_q).toBool() ? u"Публичная версия"_q : u"Непубличная версия"_q),
		st::settingsPluginCardMeta);
	AddPluginInstallLabel(card, metadata.value(u"description"_q).toString());

	const auto status = trust.value(u"status"_q).toString();
	auto warning = StatusText(status);
	auto reason = trust.value(u"reason"_q).toString();
	if (reason.isEmpty() && status == u"unverified"_q) {
		reason = u"файл ещё не проходил модерацию"_q;
	}
	if (!reason.isEmpty()) {
		warning += u" — "_q + reason;
	}
	AddPluginTrustBadge(card, std::move(warning), status);
	Ui::AddSkip(card, style::ConvertScale(4));
	return card;
}

void AddPluginInstallChoices(
		not_null<Ui::GenericBox*> box,
		not_null<Ui::VerticalLayout*> rows,
		const QJsonObject &info,
		const QString &installationId,
		rpl::variable<bool> &showDetails,
		Fn<void(QString, bool, QJsonObject)> confirm,
		Fn<void(Ui::RoundButton*)> focusCancel) {
	const auto metadata = info.value(u"metadata"_q).toObject();
	const auto trust = info.value(u"trust"_q).toObject();
	const auto status = trust.value(u"status"_q).toString();
	const auto installations = info.value(u"installations"_q).toArray();
	if (!installations.isEmpty()) {
		AddPluginInstallLabel(rows, u"Новая версия"_q, st::settingsPluginCardMeta);
	}
	const auto selected = AddPluginInstallCard(rows, metadata, trust);
	AddSeparator(selected->add(object_ptr<Ui::FixedHeightWidget>(selected, st::lineWidth)));
	const auto disclosure = selected->add(object_ptr<FileDetailsButton>(selected), style::al_justify);
	const auto technical = selected->add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		selected, object_ptr<Ui::VerticalLayout>(selected)), style::al_justify);
	AddPluginInstallLabel(technical->entity(), u"ID: "_q + metadata.value(u"id"_q).toString(),
		TechnicalTextStyle());
	AddPluginInstallLabel(technical->entity(), u"SHA-256: "_q + info.value(u"sha256"_q).toString(),
		TechnicalTextStyle());
	technical->toggleOn(showDetails.value());
	technical->finishAnimating();
	disclosure->setClickedCallback([&showDetails] { showDetails = !showDetails.current(); });
	if (status == u"blocked"_q) {
		return;
	}

	const auto eligible = [&](const QJsonObject &old) {
		return installationId.isEmpty() || installationId == old.value(u"installation_id"_q).toString();
	};
	auto hasTrusted = false;
	auto updateChoices = 0;
	for (const auto &value : installations) {
		const auto old = value.toObject();
		hasTrusted = hasTrusted || IsTrustedPlugin(old.value(u"trust"_q).toObject().value(u"status"_q).toString());
		if (eligible(old)) {
			++updateChoices;
		}
	}
	if (!installations.isEmpty()) {
		AddPluginInstallLabel(rows, u"Установленные экземпляры"_q, st::settingsPluginCardMeta);
	}
	for (const auto &value : installations) {
		const auto old = value.toObject();
		const auto id = old.value(u"installation_id"_q).toString();
		AddInstalledRow(rows, old, Version(metadata),
			updateChoices == 1 ? st::settingsPluginInstallButton : st::settingsPluginInstallSecondary,
			eligible(old) ? Fn<void()>([=] { confirm(id, false, old); }) : Fn<void()>());
	}
	if (!installationId.isEmpty() && !updateChoices) {
		AddPluginInstallLabel(rows, u"ID файла не совпадает с ID обновляемого экземпляра."_q);
		return;
	}

	if (installationId.isEmpty() && (!hasTrusted || !IsTrustedPlugin(status))) {
		const auto separate = !installations.isEmpty();
		focusCancel(SetPluginInstallAction(box,
			separate ? u"Установить отдельно"_q : u"Установить"_q,
			[=] { confirm(QString(), separate, QJsonObject()); },
			separate));
	}
}

} // namespace Ovygram
