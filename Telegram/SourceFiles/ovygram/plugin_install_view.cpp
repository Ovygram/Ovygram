#include "ovygram/plugin_install_view.h"

#include "ovygram/i18n.h"
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
#include <map>

namespace Ovygram {
namespace {

QString StatusText(const QString &status) {
	if (status == u"official"_q) return Tr(u"Official plugin"_q, u"Официальный плагин"_q);
	if (status == u"verified"_q) return Tr(u"Verified plugin"_q, u"Проверенный плагин"_q);
	if (status == u"blocked"_q) return Tr(u"Dangerous plugin — installation blocked"_q, u"Опасный плагин — установка запрещена"_q);
	if (status == u"revoked"_q) return Tr(u"Verification revoked"_q, u"Проверка отозвана"_q);
	if (status == u"verification_unavailable"_q) return Tr(u"Verification unavailable"_q, u"Проверка недоступна"_q);
	return Tr(u"Unverified"_q, u"Не проверен"_q);
}

QString Version(const QJsonObject &metadata) {
	return u"v%1"_q.arg(metadata.value(u"version"_q).toString());
}

const style::FlatLabel &TechnicalTextStyle() {
	static auto styles = std::map<int, style::FlatLabel>();
	const auto scale = style::Scale();
	const auto [entry, inserted] = styles.emplace(scale, st::settingsPluginInstallBadge);
	if (inserted) {
		entry->second.style.font = entry->second.style.font->monospace();
		entry->second.style.lineHeight = entry->second.style.font->height + style::ConvertScale(4);
	}
	return entry->second;
}

void StyleSecondaryButton(not_null<Ui::RoundButton*> button, bool onCard) {
	const auto apply = [=] {
		auto border = st::windowFg->c;
		border.setAlphaF(st::settingsPluginInstallOutlineAlpha / 100.);
		button->setBrushOverride((onCard ? st::windowBg : st::windowBgOver)->b);
		button->setPenOverride(QPen(border, st::lineWidth));
		button->setRippleOverride(st::lightButtonBgRipple->c);
	};
	apply();
	style::PaletteChanged() | rpl::on_next(apply, button->lifetime());
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
	: Ui::SettingsButton(parent, rpl::single(Tr(u"File details"_q, u"Подробности файла"_q)), st::settingsPluginInstallDetails) {
	}

	void setExpanded(bool expanded) {
		if (_expanded != expanded) {
			_expanded = expanded;
			update();
		}
	}

protected:
	void paintEvent(QPaintEvent *) override {
		auto painter = Painter(this);
		const auto over = (isOver() || isDown()) && !isDisabled();
		if (over) {
			const auto quality = PainterHighQualityEnabler(painter);
			painter.setPen(Qt::NoPen);
			painter.setBrush(st::settingsPluginRowPressed);
			if (_expanded) {
				painter.fillRect(rect(), st::settingsPluginRowPressed);
			} else {
				const auto radius = st::settingsPluginCardRadius;
				painter.drawRoundedRect(rect().adjusted(0, -radius, 0, 0), radius, radius);
			}
		}
		paintRipple(painter, 0, 0);
		paintText(painter, over, width());
		const auto &icon = st::settingsPluginInstallDetailsIcon;
		const auto &arrow = st::settingsPluginLinkArrow;
		icon.paint(painter, st().iconLeft, (height() - icon.height()) / 2, width());
		const auto arrowX = width() - st::settingsPluginTextMargin.right() - arrow.width();
		const auto arrowY = (height() - arrow.height()) / 2;
		if (_expanded) {
			painter.save();
			const auto cx = (style::RightToLeft() ? (width() - arrowX - arrow.width()) : arrowX)
				+ arrow.width() / 2.;
			const auto cy = arrowY + arrow.height() / 2.;
			painter.translate(cx, cy);
			painter.rotate(90.);
			arrow.paint(painter, -arrow.width() / 2, -arrow.height() / 2, width());
			painter.restore();
		} else {
			arrow.paint(painter, arrowX, arrowY, width());
		}
	}

private:
	bool _expanded = false;
};

void AddSeparator(not_null<Ui::RpWidget*> widget) {
	widget->paintRequest() | rpl::on_next([=] {
		auto color = st::windowFg->c;
		color.setAlphaF(st::settingsPluginDividerAlpha);
		QPainter(widget).fillRect(widget->rect(), color);
	}, widget->lifetime());
}

object_ptr<Ui::VerticalLayout> VersionBadge(QWidget *parent, const QString &version) {
	auto badge = object_ptr<Ui::VerticalLayout>(parent);
	const auto label = badge->add(
		object_ptr<Ui::FlatLabel>(badge, version, TechnicalTextStyle()),
		st::settingsPluginInstallBadgePadding);
	const auto updateColor = [=] {
		label->setTextColorOverride(st::settingsPluginLabelAccent->c);
	};
	updateColor();
	style::PaletteChanged() | rpl::on_next(updateColor, label->lifetime());
	const auto widget = badge.data();
	widget->paintRequest() | rpl::on_next([=] {
		auto painter = QPainter(widget);
		const auto quality = PainterHighQualityEnabler(painter);
		auto fill = st::activeButtonBg->c;
		fill.setAlphaF(st::settingsPluginVersionFillAlpha);
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
		bool secondaryButton,
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
	const auto rawTarget = target.data();
	const auto updateTargetColor = [=] {
		rawTarget->setTextColorOverride(PluginTrustColor(u"verified"_q));
	};
	updateTargetColor();
	style::PaletteChanged() | rpl::on_next(updateTargetColor, rawTarget->lifetime());
	delta->setTrailing(std::move(target));
	line->text->add(std::move(delta), style::al_justify);
	line->text->add(object_ptr<Ui::FlatLabel>(line->text,
		Tr(u"Current version · %1"_q, u"Текущая версия · %1"_q).arg(StatusText(status)),
		st::settingsPluginInstallBadge),
		style::al_justify);
	if (update) {
		auto button = object_ptr<Ui::RoundButton>(
			line,
			rpl::single(Tr(u"Update"_q, u"Обновить"_q)),
			secondaryButton ? st::settingsPluginInstallSecondary : st::settingsPluginInstallButton);
		button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		if (secondaryButton) {
			StyleSecondaryButton(button.data(), true);
		}
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
	box->setTitle(rpl::single(Tr(u"Install Plugin"_q, u"Установка плагина"_q)));
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
	box->addTopButton(st::settingsPluginClose, [=] { box->closeBox(); })
		->setAccessibleName(Tr(u"Close"_q, u"Закрыть"_q));
	if (!text.isEmpty()) {
		const auto button = box->addButton(rpl::single(std::move(text)),
			[callback = std::move(callback)] {
				const auto invoke = callback;
				invoke();
			},
			secondary ? st::settingsPluginInstallSecondary : st::settingsPluginInstallButton);
		button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		if (secondary) {
			StyleSecondaryButton(button, false);
		}
	}
	const auto cancel = box->addLeftButton(rpl::single(Tr(u"Cancel"_q, u"Отмена"_q)),
		[=] { box->closeBox(); }, st::settingsPluginLight);
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
		I18n::PluginName(metadata), st::settingsPluginInstallTitle);
	title->text->add(std::move(name), style::al_justify);
	title->setTrailing(VersionBadge(title, Version(metadata)));
	card->add(std::move(title), st::settingsPluginInstallRowMargin, style::al_justify);

	AddPluginInstallLabel(card, u"%1 · %2 · %3"_q.arg(
		metadata.value(u"display_author"_q).toString(metadata.value(u"author"_q).toString()),
		metadata.value(u"id"_q).toString(metadata.value(u"plugin_id"_q).toString()),
		metadata.value(u"release"_q).toBool()
			? Tr(u"Public release"_q, u"Публичная версия"_q)
			: Tr(u"Unlisted version"_q, u"Непубличная версия"_q)),
		st::settingsPluginCardMeta);
	AddPluginInstallLabel(card, I18n::PluginDescription(metadata));

	const auto status = trust.value(u"status"_q).toString();
	auto warning = StatusText(status);
	auto reason = trust.value(u"reason"_q).toString();
	if (reason.isEmpty() && status == u"unverified"_q) {
		reason = Tr(
			u"this file has not been reviewed yet"_q,
			u"файл ещё не проходил модерацию"_q);
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
		AddPluginInstallLabel(rows, Tr(u"New version"_q, u"Новая версия"_q), st::settingsPluginCardMeta);
	}
	const auto selected = AddPluginInstallCard(rows, metadata, trust);
	AddSeparator(selected->add(object_ptr<Ui::FixedHeightWidget>(selected, st::lineWidth)));
	const auto disclosure = selected->add(object_ptr<FileDetailsButton>(selected), style::al_justify);
	const auto technical = selected->add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		selected, object_ptr<Ui::VerticalLayout>(selected)), style::al_justify);
	Ui::AddSkip(technical->entity(), style::ConvertScale(8));
	AddPluginInstallLabel(technical->entity(), u"ID: "_q + metadata.value(u"id"_q).toString(),
		TechnicalTextStyle());
	AddPluginInstallLabel(technical->entity(), u"SHA-256: "_q + info.value(u"sha256"_q).toString(),
		TechnicalTextStyle());
	technical->toggleOn(showDetails.value());
	technical->finishAnimating();
	showDetails.value() | rpl::on_next([=](bool shown) {
		disclosure->setExpanded(shown);
	}, disclosure->lifetime());
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
		AddPluginInstallLabel(rows, Tr(u"Installed instances"_q, u"Установленные экземпляры"_q), st::settingsPluginCardMeta);
	}
	for (const auto &value : installations) {
		const auto old = value.toObject();
		const auto id = old.value(u"installation_id"_q).toString();
		AddInstalledRow(rows, old, Version(metadata),
			updateChoices != 1,
			eligible(old) ? Fn<void()>([=] { confirm(id, false, old); }) : Fn<void()>());
	}
	if (!installationId.isEmpty() && !updateChoices) {
		AddPluginInstallLabel(rows, Tr(
			u"The file ID does not match the instance being updated."_q,
			u"ID файла не совпадает с ID обновляемого экземпляра."_q));
		return;
	}

	if (installationId.isEmpty() && (!hasTrusted || !IsTrustedPlugin(status))) {
		const auto separate = !installations.isEmpty();
		focusCancel(SetPluginInstallAction(box,
			separate
				? Tr(u"Install separately"_q, u"Установить отдельно"_q)
				: Tr(u"Install"_q, u"Установить"_q),
			[=] { confirm(QString(), separate, QJsonObject()); },
			separate));
	}
}

} // namespace Ovygram
