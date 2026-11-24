#pragma once

#include "ui/effects/animation_value.h"
#include "ui/painter.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_plugin_management.h"
#include "styles/palette.h"

#include <algorithm>
#include <optional>

namespace Ovygram {

// The warning is a single muted badge; author/version/description retain
// Telegram's ordinary text roles. Derive the tone from the active palette.
inline QColor PluginTrustColor(const QString &status) {
	const auto warning = status != u"verified"_q && status != u"official"_q;
	auto color = status == u"official"_q
		? st::windowActiveTextFg->c
		: status == u"verified"_q
		? st::boxTextFgGood->c
		: st::attentionButtonFg->c;
	if (!warning) {
		// Telegram's night palettes map boxTextFgGood to blue, while custom
		// accents can turn active text green/purple. Trust has fixed semantics:
		// verified is green, official is blue; retain theme contrast/chroma.
		const auto dark = st::windowBg->c.lightnessF() < 0.5;
		color.setHslF(
			(status == u"verified"_q ? 125. : 210.) / 360.,
			std::clamp(double(color.hslSaturationF()), 0.48, 0.72),
			std::clamp(double(color.lightnessF()), dark ? 0.60 : 0.38, dark ? 0.72 : 0.46));
	}
	if (warning && st::windowBg->c.lightnessF() < 0.5) {
		color.setHslF(
			color.hslHueF(),
			std::min(double(color.hslSaturationF()), 0.62),
			std::max(double(color.lightnessF()), 0.66));
	}
	return color;
}

inline object_ptr<Ui::VerticalLayout> MakePluginBadge(
		QWidget *parent,
		QString text,
		std::optional<QString> trustStatus = std::nullopt) {
	auto result = object_ptr<Ui::VerticalLayout>(parent);
	const auto badge = result.data();
	const auto warning = trustStatus
		&& *trustStatus != u"verified"_q
		&& *trustStatus != u"official"_q;
	auto padding = st::settingsPluginBadgePadding;
	if (warning) {
		padding.setLeft(padding.left() + st::settingsPluginBadgeWarning.width()
			+ st::settingsPluginBadgeGap);
	}
	auto textLabel = object_ptr<Ui::FlatLabel>(badge, std::move(text), st::settingsPluginBody);
	const auto label = badge->add(std::move(textLabel), padding);
	const auto foreground = [=] {
		if (!trustStatus) {
			return st::windowSubTextFg->c;
		}
		const auto color = PluginTrustColor(*trustStatus);
		return (st::windowBg->c.lightnessF() >= 0.5)
			? anim::color(color, st::windowFg, 0.30)
			: color;
	};
	const auto updateColor = [=] {
		label->setTextColorOverride(foreground());
		badge->update();
	};
	updateColor();
	style::PaletteChanged() | rpl::on_next(updateColor, badge->lifetime());
	badge->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(badge);
		const auto quality = PainterHighQualityEnabler(p);
		auto background = trustStatus ? foreground() : st::windowFg->c;
		background.setAlpha(trustStatus ? 26 : 16);
		p.setPen(Qt::NoPen);
		p.setBrush(background);
		p.drawRoundedRect(badge->rect(), badge->height() / 2, badge->height() / 2);
		if (warning) {
			const auto &icon = st::settingsPluginBadgeWarning;
			icon.paint(p, st::settingsPluginBadgePadding.left(),
				(badge->height() - icon.height()) / 2, badge->width(), foreground());
		}
	}, badge->lifetime());
	return result;
}

inline void AddPluginTrustBadge(
		not_null<Ui::VerticalLayout*> layout,
		QString text,
		QString status) {
	layout->add(
		MakePluginBadge(layout, std::move(text), std::move(status)),
		st::settingsPluginTextMargin);
}

} // namespace Ovygram
