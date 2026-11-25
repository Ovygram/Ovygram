#include "ovygram/plugin_ui_tree.h"

#include "ovygram/plugin_paint.h"
#include "ui/effects/animations.h"
#include "ui/effects/ripple_animation.h"
#include "ui/layers/box_layer_widget.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_plugin_management.h"
#include "styles/style_widgets.h"

#include <QtCore/QJsonArray>
#include <QtCore/QRegularExpression>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtGui/QPaintEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <vector>

namespace Ovygram {
namespace {

constexpr auto kMaximumDepth = 32;
constexpr auto kMaximumNodes = 2000;
constexpr auto kMaximumCanvasHeight = 4096;
constexpr auto kMaximumCanvasOps = 2000;
constexpr auto kRowSkip = 8;
constexpr auto kColumnSkip = 6;

// st::boxLabel with another alignment. Kept in static storage, because
// FlatLabel keeps a reference to its style.
[[nodiscard]] const style::FlatLabel &LabelStyle(style::align align, bool bold) {
	static auto styles = std::map<std::pair<int, bool>, style::FlatLabel>();
	const auto [entry, inserted] = styles.emplace(std::pair(int(align), bold), st::boxLabel);
	if (inserted) {
		entry->second.align = align;
		entry->second.minWidth = 1;
		if (bold) entry->second.style.font = entry->second.style.font->bold();
	}
	return entry->second;
}

[[nodiscard]] std::optional<QColor> ReadColor(const QJsonValue &value) {
	if (value.isUndefined() || value.isNull()) {
		return std::nullopt;
	}
	const auto text = value.toString();
	static const auto pattern = QRegularExpression(u"^#[0-9A-Fa-f]{6}$"_q);
	if (text.size() != 7 || !pattern.match(text).hasMatch()) {
		throw std::runtime_error("A plugin color must use #RRGGBB.");
	}
	return QColor(text);
}

class PaletteLabel final : public Ui::FlatLabel {
public:
	PaletteLabel(QWidget *parent, const QString &text, const style::FlatLabel &style)
	: FlatLabel(parent, text, style) {
		style::PaletteChanged() | rpl::on_next([this] { applyColor(); }, lifetime());
	}

	void setTone(QString tone) {
		_tone = std::move(tone);
		applyColor();
	}

	void setColor(std::optional<QColor> color) {
		_color = std::move(color);
		applyColor();
	}

private:
	void applyColor() {
		auto color = _color;
		if (!color) {
			if (_tone == u"muted"_q) color = st::settingsPluginLabelMuted->c;
			else if (_tone == u"accent"_q) color = st::settingsPluginLabelAccent->c;
			else if (_tone == u"positive"_q) color = st::settingsPluginLabelPositive->c;
			else if (_tone == u"attention"_q) color = st::settingsPluginLabelAttention->c;
		}
		setTextColorOverride(color);
	}

	QString _tone;
	std::optional<QColor> _color;
};

[[nodiscard]] const style::RoundButton &ButtonStyle(const QString &variant) {
	if (variant == u"primary"_q || variant == u"native_primary"_q) return st::settingsPluginPrimary;
	if (variant == u"install"_q) return st::settingsPluginInstallButton;
	if (variant == u"subtle"_q || variant == u"install_secondary"_q) return st::settingsPluginInstallSecondary;
	if (variant == u"proxy_selector"_q) return st::settingsPluginProxySelector;
	if (variant == u"proxy_reset"_q) return st::settingsPluginProxyReset;
	if (variant == u"dialog"_q) return st::settingsPluginDialog;
	if (variant == u"dialog_danger"_q) return st::settingsPluginDialogDanger;
	if (variant == u"chip"_q) {
		static const auto chip = [] {
			auto style = st::settingsPluginLight;
			style.height = st::settingsPluginChipHeight;
			style.width = st::settingsPluginChipWidth;
			style.textTop = std::max((style.height - style.style.font->height) / 2, 0);
			style.textBg = st::windowBgOver;
			style.textFg = st::windowBoldFg;
			return style;
		}();
		return chip;
	}
	if (variant != u"code"_q) return st::settingsPluginLight;
	static const auto code = [] {
		auto style = st::settingsPluginLight;
		style.style.font = style.style.font->monospace();
		style.height = st::settingsPluginCodeHeight;
		style.textTop = std::max((style.height - style.style.font->height) / 2, 0);
		style.textBg = st::windowBg;
		return style;
	}();
	return code;
}

[[nodiscard]] bool DarkButtonPalette() {
	return st::windowBg->c.lightness() < 128;
}

struct ButtonOverrides {
	std::optional<QColor> text;
	std::optional<QColor> background;
	std::optional<QColor> border;
	std::optional<QColor> hoverText;
	std::optional<QColor> hoverBackground;
	std::optional<QColor> ripple;
};

[[nodiscard]] ButtonOverrides ReferenceColors(const QString &variant) {
	if (variant == u"primary"_q) return {
		.text = QColor(st::settingsPluginRefPrimaryText),
		.background = QColor(st::settingsPluginRefPrimaryFill),
	};
	if (variant == u"secondary"_q) return {
		.text = QColor(st::settingsPluginRefSecondaryText),
		.background = QColor(st::settingsPluginRefSecondaryFill),
		.border = QColor(st::settingsPluginRefSecondaryBorder),
	};
	if (variant == u"danger"_q || variant == u"trash"_q) return {
		.text = QColor(st::settingsPluginRefDangerText),
		.background = QColor(st::settingsPluginRefDangerFill),
	};
	if (variant == u"code"_q) return {
		.text = QColor(st::settingsPluginRefCodeText),
		.background = QColor(st::settingsPluginRefCodeFill),
	};
	return {};
}

[[nodiscard]] ButtonOverrides ReadButtonOverrides(const QJsonValue &value) {
	if (value.isNull() || value.isUndefined()) {
		return {};
	}
	if (!value.isObject()) {
		throw std::runtime_error("Button colors must be an object.");
	}
	const auto colors = value.toObject();
	return {
		.text = ReadColor(colors.value(u"text"_q)),
		.background = ReadColor(colors.value(u"background"_q)),
		.border = ReadColor(colors.value(u"border"_q)),
		.hoverText = ReadColor(colors.value(u"hover_text"_q)),
		.hoverBackground = ReadColor(colors.value(u"hover_background"_q)),
		.ripple = ReadColor(colors.value(u"ripple"_q)),
	};
}

[[nodiscard]] std::optional<int> ReadButtonRadius(const QJsonValue &value) {
	if (value.isUndefined() || value.isNull()) return std::nullopt;
	const auto radius = value.toInt(-1);
	if (radius < 0 || radius > 64) {
		throw std::runtime_error("Button radius must be a whole number from 0 to 64.");
	}
	return radius;
}

[[nodiscard]] int ScaledRadius(int radius) {
	// ConvertScale rounds very small distances up to one. Zero means square.
	return radius ? style::ConvertScale(radius) : 0;
}

class ColorableButton {
public:
	virtual ~ColorableButton() = default;
	virtual void setColors(ButtonOverrides colors) = 0;
	virtual void setRadius(std::optional<int> radius) = 0;
};

[[nodiscard]] ButtonOverrides RoundColors(const QString &variant, bool useOvygramPalette) {
	auto colors = useOvygramPalette && DarkButtonPalette() ? ReferenceColors(variant) : ButtonOverrides();
	if (colors.text) {
		colors.ripple = colors.text;
		colors.ripple->setAlpha(st::settingsPluginSdkRippleAlpha);
	} else if (variant == u"danger"_q) {
		colors.text = st::settingsPluginLabelAttention->c;
		colors.background = colors.text;
		colors.background->setAlpha(st::settingsPluginSdkDangerAlpha);
	} else if (variant == u"secondary"_q) {
		colors.background = st::windowBg->c;
		colors.border = st::lightButtonFg->c;
		colors.border->setAlpha(st::settingsPluginSdkBorderAlpha);
		colors.ripple = colors.border;
	} else if (variant == u"install_secondary"_q) {
		colors.background = st::settingsPluginInstallSecondary.textBg->c;
		colors.border = st::windowFg->c;
		colors.border->setAlphaF(st::settingsPluginInstallOutlineAlpha / 100.);
		colors.ripple = st::settingsPluginInstallSecondary.ripple.color->c;
	}
	return colors;
}

[[nodiscard]] QPainterPath ButtonShape(QRectF bounds, int radius) {
	auto path = QPainterPath();
	path.addRoundedRect(bounds, radius, radius);
	return path;
}

[[nodiscard]] QColor FilledButtonHoverColor(QColor fill, QColor text) {
	return anim::color(fill, text, st::settingsPluginSdkHoverBlend);
}

class PaletteRoundButton final : public Ui::RoundButton, public ColorableButton {
public:
	PaletteRoundButton(
		QWidget *parent,
		rpl::producer<QString> text,
		QString variant,
		bool useOvygramPalette)
	: RoundButton(parent, std::move(text), ButtonStyle(variant))
	, _variant(std::move(variant))
	, _useOvygramPalette(useOvygramPalette) {
		setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		if (_variant == u"chip"_q) {
			setFullRadius(true);
		}
		style::PaletteChanged() | rpl::on_next([this] { applyColors(); }, lifetime());
	}

	void setColors(ButtonOverrides colors) override {
		_colors = std::move(colors);
		applyColors();
	}

	void setRadius(std::optional<int> radius) override {
		if (_radius == radius) return;
		_radius = radius;
		finishAnimating();
		applyColors();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		if (customShape()) {
			auto p = QPainter(this);
			const auto quality = PainterHighQualityEnabler(p);
			const auto bounds = rect().marginsRemoved(st().padding);
			const auto over = Highlighted(state());
			const auto fill = _fill.value_or(st().textBg->c);
			p.setBrush(_hoverFill
				? anim::color(fill, *_hoverFill, _hoverAnimation.value(over ? 1. : 0.))
				: over ? st().textBgOver->c : fill);
			p.setPen(_border ? QPen(*_border, _borderWidth) : QPen(Qt::NoPen));
			const auto corners = cornerRadius(bounds);
			p.drawRoundedRect(myrtlrect(bounds), corners, corners);
		}
		RoundButton::paintEvent(e);
	}

	QImage prepareRippleMask() const override {
		if (!_radius) return RoundButton::prepareRippleMask();
		const auto bounds = rect().marginsRemoved(st().padding);
		return Ui::RippleAnimation::RoundRectMask(bounds.size(), cornerRadius(bounds));
	}

	void onStateChanged(State was, StateChangeSource source) override {
		RoundButton::onStateChanged(was, source);
		const auto over = Highlighted(state());
		if (_hoverFill && over != Highlighted(was)) {
			_hoverAnimation.start([this] { update(); },
				_hoverAnimation.value(over ? 0. : 1.), over ? 1. : 0.,
				st::settingsPluginSdkHoverDuration);
		}
		if (_colors.hoverText) applyText();
	}

private:
	[[nodiscard]] static bool Highlighted(State state) {
		return (state & (StateFlag::Over | StateFlag::Down))
			&& !(state & StateFlag::Disabled);
	}

	[[nodiscard]] bool customShape() const {
		return _radius.has_value() || _hoverFill.has_value();
	}

	[[nodiscard]] int cornerRadius(QRect bounds) const {
		return _radius ? ScaledRadius(*_radius) : (_variant == u"chip"_q)
			? bounds.height() / 2
			: st().radius;
	}

	void applyText() {
		setTextFgOverride(Highlighted(state()) && _colors.hoverText ? _colors.hoverText : _text);
	}

	void applyColors() {
		const auto role = RoundColors(_variant, _useOvygramPalette);
		_text = _colors.text ? _colors.text : role.text;
		_fill = _colors.background ? _colors.background : role.background;
		_border = _colors.border ? _colors.border : role.border;
		if (_border && !_fill) _fill = st().textBg->c;
		_hoverFill = _colors.hoverBackground;
		if (!_hoverFill && _fill) {
			_hoverFill = FilledButtonHoverColor(*_fill, _text.value_or(st().textFg->c));
		}
		if (!_hoverFill) _hoverAnimation.stop();
		auto ripple = role.ripple;
		if (_colors.ripple || _colors.text || _colors.border) {
			ripple = _colors.ripple ? _colors.ripple : _colors.text ? _colors.text : _colors.border;
			ripple->setAlpha(st::settingsPluginSdkRippleAlpha);
		}
		_borderWidth = (_colors.border || (_useOvygramPalette && DarkButtonPalette())) ? 2. : 1.;
		if (_variant == u"install_secondary"_q && !_colors.border) _borderWidth = st::lineWidth;
		applyText();
		setBrushOverride(customShape()
			? std::make_optional(QBrush(Qt::transparent))
			: _fill ? std::make_optional(QBrush(*_fill)) : std::nullopt);
		// The outer half of the stroke is clipped at the widget edge.
		setPenOverride(!customShape() && _border
			? std::make_optional(QPen(*_border, _borderWidth))
			: std::nullopt);
		setRippleOverride(ripple);
	}

	const QString _variant;
	const bool _useOvygramPalette;
	ButtonOverrides _colors;
	std::optional<QColor> _text;
	std::optional<QColor> _fill;
	std::optional<QColor> _hoverFill;
	std::optional<QColor> _border;
	std::optional<int> _radius;
	Ui::Animations::Simple _hoverAnimation;
	qreal _borderWidth = 1.;
};

[[nodiscard]] const style::IconButton *IconStyle(const QString &variant) {
	if (variant == u"back"_q) return &st::settingsPluginBack;
	if (variant == u"close"_q || variant == u"trash"_q) return &st::settingsPluginClose;
	return nullptr;
}

class PaletteIconButton final : public Ui::IconButton, public ColorableButton {
public:
	PaletteIconButton(QWidget *parent, const QString &variant, bool useOvygramPalette)
	: IconButton(parent, *IconStyle(variant))
	, _trash(variant == u"trash"_q)
	, _useOvygramPalette(useOvygramPalette) {
		if (_trash) {
			setIconOverride(&st::menuIconDeleteAttention, &st::menuIconDeleteAttention);
		}
		style::PaletteChanged() | rpl::on_next([this] { applyColors(); }, lifetime());
	}

	void setColors(ButtonOverrides colors) override {
		_colors = std::move(colors);
		applyColors();
	}

	void setRadius(std::optional<int> radius) override {
		if (_radius == radius) return;
		_radius = radius;
		finishAnimating();
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		const auto over = isOver() || isDown();
		const auto reference = referenceColors();
		auto background = over && _colors.hoverBackground ? _colors.hoverBackground : _colors.background;
		if (!background) background = reference.background;
		if (background || _colors.border) {
			auto p = QPainter(this);
			const auto quality = PainterHighQualityEnabler(p);
			p.setPen(_colors.border ? QPen(*_colors.border) : QPen(Qt::NoPen));
			p.setBrush(background ? QBrush(*background) : QBrush(Qt::NoBrush));
			p.drawPath(ButtonShape(faceRect().adjusted(.5, .5, -.5, -.5),
				_radius ? ScaledRadius(*_radius) : st::settingsPluginRowRadius));
		}
		IconButton::paintEvent(e);
	}

	QImage prepareRippleMask() const override {
		if (!_radius) return IconButton::prepareRippleMask();
		const auto size = QSize(st().rippleAreaSize, st().rippleAreaSize);
		return Ui::RippleAnimation::MaskByDrawer(size, false, [&](QPainter &p) {
			p.drawPath(ButtonShape(faceRect().translated(-st().rippleAreaPosition), ScaledRadius(*_radius)));
		});
	}

	void onStateChanged(State was, StateChangeSource source) override {
		IconButton::onStateChanged(was, source);
		if (_colors.hoverText) applyIcon();
	}

private:
	[[nodiscard]] QRectF faceRect() const {
		const auto h = _trash ? std::min(height(), st::settingsPluginPrimary.height) : height();
		return QRectF(0, (height() - h) / 2., width(), h);
	}

	[[nodiscard]] ButtonOverrides referenceColors() const {
		return _trash && _useOvygramPalette && DarkButtonPalette()
			? ReferenceColors(u"trash"_q)
			: ButtonOverrides();
	}

	void applyIcon() {
		auto text = (isOver() || isDown()) && _colors.hoverText ? _colors.hoverText : _colors.text;
		if (!text) text = referenceColors().text;
		setIconColorOverride(text);
	}

	void applyColors() {
		applyIcon();
		if (_colors.ripple) {
			auto color = *_colors.ripple;
			color.setAlpha(st::settingsPluginSdkRippleAlpha);
			_ripple.update(color);
		}
		setRippleColorOverride(_colors.ripple ? &_ripple.color() : nullptr);
		update();
	}

	const bool _trash;
	const bool _useOvygramPalette;
	ButtonOverrides _colors;
	std::optional<int> _radius;
	style::owned_color _ripple{ QColor() };
};

struct SearchClearAppearance {
	style::CrossButton style = st::settingsPluginSearchClear;
	style::owned_color text{ QColor() };
	style::owned_color hoverText{ QColor() };
	style::owned_color ripple{ QColor() };
};

class PaletteSearchClear final
	: private SearchClearAppearance
	, public Ui::CrossButton
	, public ColorableButton {
public:
	explicit PaletteSearchClear(QWidget *parent)
	: CrossButton(parent, SearchClearAppearance::style) {
		show(anim::type::instant);
	}

	void setColors(ButtonOverrides colors) override {
		_colors = std::move(colors);
		const auto &original = st::settingsPluginSearchClear;
		auto &appearance = SearchClearAppearance::style;
		text.update(_colors.text.value_or(original.crossFg->c));
		hoverText.update(_colors.hoverText.value_or(_colors.text.value_or(original.crossFgOver->c)));
		appearance.crossFg = _colors.text ? text.color() : original.crossFg;
		appearance.crossFgOver = (_colors.text || _colors.hoverText) ? hoverText.color() : original.crossFgOver;
		if (_colors.ripple) {
			auto color = *_colors.ripple;
			color.setAlpha(st::settingsPluginSdkRippleAlpha);
			ripple.update(color);
		}
		appearance.ripple.color = _colors.ripple ? ripple.color() : original.ripple.color;
		update();
	}

	void setRadius(std::optional<int> radius) override {
		if (_radius == radius) return;
		_radius = radius;
		Ui::RippleButton::finishAnimating();
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		const auto fill = (isOver() || isDown()) && _colors.hoverBackground
			? _colors.hoverBackground : _colors.background;
		if (fill || _colors.border) {
			auto p = QPainter(this);
			const auto quality = PainterHighQualityEnabler(p);
			p.setBrush(fill ? QBrush(*fill) : QBrush(Qt::NoBrush));
			p.setPen(_colors.border ? QPen(*_colors.border) : QPen(Qt::NoPen));
			p.drawPath(ButtonShape(QRectF(rect()).adjusted(.5, .5, -.5, -.5),
				_radius ? ScaledRadius(*_radius) : st::settingsPluginRowRadius));
		}
		CrossButton::paintEvent(e);
	}

	QImage prepareRippleMask() const override {
		if (!_radius) return CrossButton::prepareRippleMask();
		const auto size = SearchClearAppearance::style.cross.size;
		return Ui::RippleAnimation::RoundRectMask(QSize(size, size), ScaledRadius(*_radius));
	}

private:
	ButtonOverrides _colors;
	std::optional<int> _radius;
};

[[nodiscard]] const style::SettingsButton *RowStyle(const QString &variant) {
	if (variant == u"card_title"_q) return &st::settingsPluginCardTitle;
	if (variant == u"card_action"_q) return &st::settingsPluginCardAction;
	if (variant == u"card_danger"_q) return &st::settingsPluginCardRemove;
	if (variant == u"save_settings"_q) return &st::settingsPluginSave;
	if (variant == u"settings_link"_q) return &st::settingsPluginLinkRow;
	if (variant == u"file_details"_q) return &st::settingsPluginInstallDetails;
	if (variant == u"menu_action"_q) return &st::settingsPluginMenuRow;
	if (variant == u"menu_danger"_q) return &st::settingsPluginMenuRemove;
	if (variant == u"popup_item"_q) return &st::settingsPluginPopupRow;
	if (variant == u"settings_row"_q || variant == u"link"_q) return &st::settingsPluginPlainRow;
	return nullptr;
}

[[nodiscard]] const style::icon *MenuRowIcon(const QString &variant) {
	if (variant == u"menu_action"_q) return &st::settingsPluginRollbackIcon;
	if (variant == u"menu_danger"_q) return &st::settingsPluginRemoveIcon;
	return nullptr;
}

[[nodiscard]] bool HasRowArrow(const QString &variant) {
	return variant == u"card_title"_q || variant == u"settings_link"_q || variant == u"file_details"_q;
}

class PaletteSettingsButton : public Ui::SettingsButton, public ColorableButton {
public:
	PaletteSettingsButton(QWidget *parent, rpl::producer<QString> text,
		const style::SettingsButton &style, QString variant = {}, bool fit = false)
	: SettingsButton(parent, std::move(text), style)
	, _variant(std::move(variant))
	, _menuIcon(MenuRowIcon(_variant))
	, _menuIconLeft(fit && _menuIcon ? style.padding.right() : style.iconLeft) {
		if (!_variant.isEmpty()) {
			auto padding = st().padding;
			if (fit && _menuIcon) {
				// Preserve the space between the icon and its text.
				padding.setLeft(padding.left() + _menuIconLeft - st().iconLeft);
			}
			const auto arrowArea = (_variant == u"link"_q)
				? st::settingsPluginSdkLinkArrowInset + st::settingsPluginSdkLinkArrowSize
				: HasRowArrow(_variant) ? st::settingsPluginTextMargin.right() + st::settingsPluginLinkArrow.width()
				: 0;
			if (arrowArea) {
				padding.setRight(std::max(padding.right(), arrowArea + st::settingsPluginSdkArrowGap));
			}
			if (padding != st().padding) setPaddingOverride(padding);
			_horizontalPadding = padding.left() + padding.right();
			refreshNaturalWidth();
		}
	}

	void refreshNaturalWidth() {
		if (!_variant.isEmpty()) setNaturalWidth(fullTextWidth() + _horizontalPadding);
	}

	void setColors(ButtonOverrides colors) override {
		_colors = std::move(colors);
		applyText();
		update();
	}

	void setRadius(std::optional<int> radius) override {
		if (_radius == radius) return;
		_radius = radius;
		finishAnimating();
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		const auto over = (isOver() || isDown()) && !isDisabled();
		const auto centerText = _variant == u"card_action"_q || _variant == u"card_danger"_q;
		const auto cardAction = centerText || _variant == u"card_title"_q;
		const auto settingsLink = _variant == u"settings_link"_q;
		const auto fileDetails = _variant == u"file_details"_q;
		if (!_colors.background && !_colors.hoverBackground && !_colors.border && !_colors.ripple
			&& !_radius && !cardAction && !settingsLink && !fileDetails && !_menuIcon) {
			SettingsButton::paintEvent(e);
			return;
		}
		auto p = Painter(this);
		auto fill = (over ? _colors.hoverBackground : std::nullopt).value_or(
			_colors.background.value_or((over ? st().textBgOver : st().textBg)->c));
		auto bounds = QRectF(rect());
		auto radius = _radius ? ScaledRadius(*_radius) : 0;
		if (settingsLink || fileDetails) {
			if (!_colors.background && !(over && _colors.hoverBackground)) {
				fill = settingsLink ? (over ? st::settingsPluginRowPressed : st::settingsPluginRowHover)->c
					: over ? st::settingsPluginRowPressed->c : st::transparent->c;
			}
			if (!_radius) radius = st::settingsPluginCardRadius;
			if (fileDetails) bounds.adjust(0, -radius, 0, 0);
		} else if (cardAction && over && !_colors.background && !_colors.hoverBackground) {
			fill = st::settingsPluginCardActionOverlay->c;
			fill.setAlpha(isDown() ? st::settingsPluginCardActionPressedAlpha : st::settingsPluginCardActionHoverAlpha);
			bounds.adjust(0, 4, 0, -4);
			if (!_radius) radius = st::settingsPluginLegacyCardActionRadius;
		}
		paintSurface(p, bounds, radius, fill);
		if (centerText) {
			const auto spare = width() - st().padding.left() - st().padding.right() - fullTextWidth();
			const auto offset = std::max(spare, 0) / 2;
			p.save();
			p.translate(style::RightToLeft() ? -offset : offset, 0);
			paintText(p, over, width());
			p.restore();
		} else {
			paintText(p, over, width());
		}
		const auto icon = settingsLink ? &st::settingsPluginSettingsIcon
			: fileDetails ? &st::settingsPluginInstallDetailsIcon : _menuIcon;
		if (icon) {
			icon->paint(p, _menuIconLeft, (height() - icon->height()) / 2, width());
		}
		if (HasRowArrow(_variant)) {
			const auto &arrow = st::settingsPluginLinkArrow;
			const auto left = width() - st::settingsPluginTextMargin.right() - arrow.width();
			if (left >= 0) {
				arrow.paint(p, left, (height() - arrow.height()) / 2, width());
			}
		}
	}

	void paintSurface(Painter &p, QRectF bounds, int radius, QColor fill) {
		const auto quality = PainterHighQualityEnabler(p);
		const auto path = ButtonShape(bounds, radius);
		p.fillPath(path, fill);
		p.save();
		if (radius) p.setClipPath(path);
		auto ripple = _colors.ripple;
		if (ripple) ripple->setAlpha(st::settingsPluginSdkRippleAlpha);
		paintRipple(p, 0, 0, ripple ? &*ripple : nullptr);
		p.restore();
		if (_colors.border) {
			p.setPen(*_colors.border);
			p.setBrush(Qt::NoBrush);
			p.drawPath(ButtonShape(bounds.adjusted(.5, .5, -.5, -.5), radius));
		}
	}

	void onStateChanged(State was, StateChangeSource source) override {
		SettingsButton::onStateChanged(was, source);
		if (_colors.hoverText) applyText();
	}

	void applyText() {
		const auto over = (isOver() || isDown()) && !isDisabled();
		setColorOverride(over && _colors.hoverText ? _colors.hoverText : _colors.text);
	}

	ButtonOverrides _colors;
	std::optional<int> _radius;

private:
	const QString _variant;
	const style::icon *const _menuIcon;
	const int _menuIconLeft;
	int _horizontalPadding = 0;
};

class ChoiceButton final : public PaletteSettingsButton {
public:
	ChoiceButton(QWidget *parent, rpl::producer<QString> text)
	: PaletteSettingsButton(parent, std::move(text), Style()) {
	}

	void setDetail(QString detail) {
		_detail = std::move(detail);
		auto padding = st().padding;
		padding.setRight(padding.right() + (_detail.isEmpty()
			? 0
			: st().style.font->width(_detail) + padding.left()));
		setPaddingOverride(padding);
		accessibilityNameChanged();
		update();
	}

	void setSelected(bool selected) {
		_selected = selected;
		update();
	}

	QString accessibilityName() override {
		return Ui::SettingsButton::accessibilityName()
			+ (_detail.isEmpty() ? QString() : u", "_q + _detail);
	}

protected:
	void paintEvent(QPaintEvent *) override {
		auto p = Painter(this);
		const auto over = isOver() && !isDisabled();
		const auto fill = _selected ? st::settingsPluginChoiceSelected
			: over ? st::settingsPluginRowPressed : st::settingsPluginRowHover;
		const auto background = (over ? _colors.hoverBackground : std::nullopt)
			.value_or(_colors.background.value_or(fill->c));
		const auto radius = _radius ? ScaledRadius(*_radius) : st::settingsPluginRowRadius;
		if (_radius || _colors.hoverBackground || _colors.ripple) {
			paintSurface(p, rect(), radius, background);
		} else {
			const auto quality = PainterHighQualityEnabler(p);
			p.setPen(_colors.border ? QPen(*_colors.border) : QPen(Qt::NoPen));
			p.setBrush(background);
			p.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), radius, radius);
			paintRipple(p, 0, 0);
		}
		paintText(p, _selected, width());
		if (!_detail.isEmpty()) {
			const auto font = st().style.font;
			p.setFont(font);
			p.setPen((over ? _colors.hoverText : std::nullopt).value_or(
				_colors.text.value_or((_selected ? st::windowFgActive : st::windowSubTextFg)->c)));
			p.drawTextRight(st().padding.right(), (height() - font->height) / 2,
				width(), _detail);
		}
	}

private:
	static const style::SettingsButton &Style() {
		static const auto result = [] {
			auto style = st::settingsPluginChoice;
			style.height = std::max(style.style.font->height, st::settingsPluginChoiceHeight);
			return style;
		}();
		return result;
	}

	QString _detail;
	bool _selected = false;
};

class SearchField final : public Ui::InputField {
public:
	SearchField(QWidget *parent, QString placeholder, QString text)
	: InputField(parent, Style(), Mode::SingleLine, rpl::single(std::move(placeholder)),
		TextWithTags{ std::move(text) }) {
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		InputField::paintEvent(e);
		auto p = QPainter(this);
		const auto &icon = st::menuIconSearch;
		icon.paint(p, style::ConvertScale(9), (height() - icon.height()) / 2, width());
	}

private:
	static const style::InputField &Style() {
		static const auto result = [] {
			auto style = st::defaultMultiSelectSearchField;
			const auto left = st::menuIconSearch.width() + style::ConvertScale(16);
			const auto top = style::ConvertScale(9);
			style.heightMin = style::ConvertScale(34);
			style.textMargins = QMargins(left, top, style::ConvertScale(9), 0);
			style.placeholderMargins = QMargins();
			style.textBg = style.textBgActive = st::windowBg;
			style.border = style::ConvertScale(1);
			style.borderRadius = style::ConvertScale(6);
			return style;
		}();
		return result;
	}
};

class ContentFrame final : public Ui::RpWidget {
public:
	ContentFrame(QWidget *parent, object_ptr<Ui::RpWidget> child, int padding, int cap, bool card)
	: RpWidget(parent)
	, _child(child.release())
	, _padding(padding)
	, _cap(cap)
	, _card(card) {
		_child->setParent(this);
		_child->show();
		_child->heightValue() | rpl::skip(1) | rpl::on_next([=] {
			if (!_resizing) resizeToWidth(width());
		}, lifetime());
	}

	void setBackground(std::optional<QColor> background) {
		_background = std::move(background);
		update();
	}

protected:
	int resizeGetHeight(int width) override {
		_resizing = true;
		const auto guard = gsl::finally([&] { _resizing = false; });
		const auto outer = _cap ? std::min(width, _cap) : width;
		const auto margins = _child->getMargins();
		_child->resizeToWidth(std::max(outer - _padding * 2 - margins.left() - margins.right(), 0));
		_child->move((width - _child->width()) / 2, _padding);
		return _child->height() + _padding * 2;
	}
	void paintEvent(QPaintEvent *) override {
		if (!_card) return;
		auto p = QPainter(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		auto background = st::windowBgOver->c;
		background.setAlpha(125);
		p.setBrush(_background.value_or(background));
		p.drawRoundedRect(rect(), style::ConvertScale(12), style::ConvertScale(12));
	}
private:
	const not_null<Ui::RpWidget*> _child;
	const int _padding;
	const int _cap;
	const bool _card;
	std::optional<QColor> _background;
	bool _resizing = false;
};

class Meter final : public Ui::RpWidget {
public:
	explicit Meter(QWidget *parent) : RpWidget(parent) { resize(1, style::ConvertScale(5)); }
	void setValue(double value) {
		_value = std::clamp(value, 0., 1.);
		update();
	}
protected:
	void paintEvent(QPaintEvent *) override {
		auto p = QPainter(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(st::shadowFg);
		p.drawRoundedRect(rect(), height() / 2., height() / 2.);
		if (_value > 0.) {
			p.setBrush(st::windowBgActive);
			p.drawRoundedRect(QRectF(0, 0, width() * _value, height()), height() / 2., height() / 2.);
		}
	}
private:
	double _value = 0.;
};

class PluginSlider final : public Ui::MediaSlider {
public:
	PluginSlider(QWidget *parent, int minimum, int maximum, Fn<void(int)> changed)
	: MediaSlider(parent, st::defaultContinuousSlider)
	, _minimum(minimum)
	, _span(maximum - minimum)
	, _changed(std::move(changed)) {
		setAlwaysDisplayMarker(true);
		setFocusPolicy(Qt::StrongFocus);
		resize(1, std::max(st::defaultContinuousSlider.seekSize.height(), style::ConvertScale(24)));
		setChangeProgressCallback([this](double value) { report(value); });
		setChangeFinishedCallback([this](double value) { report(value); });
	}

	void setPluginValue(int value) {
		_lastReported = std::clamp(value, _minimum, _minimum + _span);
		setValue(double(_lastReported - _minimum) / _span);
	}

	QString accessibilityValue() const override {
		return QString::number(toInteger(value()));
	}

protected:
	void mouseMoveEvent(QMouseEvent *e) override {
		MediaSlider::mouseMoveEvent(e);
		if (isChanging()) report(value());
	}

private:
	[[nodiscard]] int toInteger(double value) const {
		return _minimum + int(std::round(value * _span));
	}

	void report(double value) {
		const auto current = toInteger(value);
		if (current != _lastReported) {
			_lastReported = current;
			_changed(current);
		}
	}

	const int _minimum;
	const int _span;
	const Fn<void(int)> _changed;
	int _lastReported = 0;
};

class Canvas final : public Ui::RpWidget {
public:
	void setHeight(int height) {
		_height = std::clamp(height, 1, kMaximumCanvasHeight);
	}

	Canvas(QWidget *parent, int height, Fn<void(int, int)> clicked)
	: RpWidget(parent)
	, _height(std::clamp(height, 1, kMaximumCanvasHeight))
	, _clicked(std::move(clicked)) {
		resize(width(), _height);
	}

	void setCommands(const QJsonArray &commands) {
		_ops = ParsePaintOps(commands, kMaximumCanvasOps);
		update();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return _height;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		p.setRenderHint(QPainter::Antialiasing);
		PaintOps(p, _ops, rect());
	}

	void mousePressEvent(QMouseEvent *e) override {
		if (_clicked && e->button() == Qt::LeftButton) {
			_clicked(e->pos().x(), e->pos().y());
		}
	}

private:
	int _height = 0;
	const Fn<void(int, int)> _clicked;
	std::vector<PaintOp> _ops;

};

class Picture final : public Ui::RpWidget {
public:
	Picture(QWidget *parent, QImage image, int height)
	: RpWidget(parent)
	, _image(std::move(image))
	, _height(height > 0 ? height : _image.height()) {
		resize(width(), _height);
	}

protected:
	int resizeGetHeight(int newWidth) override {
		return _height;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		const auto scaled = _image.size().scaled(
			size(),
			Qt::KeepAspectRatio);
		p.drawImage(QRect(QPoint(), scaled), _image);
	}

private:
	const QImage _image;
	const int _height = 0;

};

// Lays children out horizontally. Each child has a fixed width, its natural
// width ("fit") or a share of the remaining width ("stretch"), and the
// children are aligned vertically inside the row.
class Row final : public Ui::RpWidget {
public:
	struct Size {
		int fixed = -1;
		bool fit = false;
		int stretch = 1;
	};

	Row(QWidget *parent, Qt::Alignment align, int spacing, bool wrap = false)
	: RpWidget(parent)
	, _align(align)
	, _spacing(spacing)
	, _wrap(wrap) {
	}

	void add(object_ptr<Ui::RpWidget> child, Size size, bool visible) {
		const auto raw = child.release();
		raw->setParent(this);
		raw->setVisible(visible);
		_children.push_back({ raw, size });
		raw->heightValue(
		) | rpl::skip(1) | rpl::on_next([=] {
			if (!_resizing) {
				resizeToWidth(width());
			}
		}, lifetime());
	}

protected:
	int resizeGetHeight(int newWidth) override {
		std::erase_if(_children, [](const Child &child) {
			return !child.widget;
		});
		auto children = std::vector<Child>();
		for (const auto &child : _children) {
			if (!child.widget->isHidden()) {
				children.push_back(child);
			}
		}
		const auto count = int(children.size());
		if (!count) {
			return 0;
		}
		_resizing = true;
		const auto guard = gsl::finally([&] { _resizing = false; });
		if (_wrap) {
			auto desired = std::vector<int>();
			desired.reserve(count);
			auto total = _spacing * (count - 1);
			for (const auto &child : children) {
				const auto width = child.size.fixed >= 0
					? child.size.fixed
					: natural(child.widget.data());
				desired.push_back(width);
				total += width;
			}
			if (total > newWidth) {
				return layoutWrapped(children, desired, std::max(newWidth, 1));
			}
		}
		const auto spacing = (count > 1)
			? std::min(_spacing, std::max(newWidth - count, 0) / (count - 1))
			: 0;
		const auto available = std::max(newWidth - spacing * (count - 1), 0);
		auto widths = std::vector<int>(count, -1);
		auto used = 0;
		auto weights = 0;
		for (auto i = 0; i != count; ++i) {
			const auto &[widget, size] = children[i];
			if (size.fixed >= 0) {
				widths[i] = size.fixed;
			} else if (size.fit || size.stretch <= 0) {
				widths[i] = natural(widget.data());
			} else {
				weights += size.stretch;
				continue;
			}
			used += widths[i];
		}
		auto remaining = std::max(available - used, 0);
		auto reserved = std::min(available, used);
		for (auto i = 0; i != count; ++i) {
			if (widths[i] < 0) {
				const auto stretch = children[i].size.stretch;
				widths[i] = weights ? (remaining * stretch / weights) : 0;
				remaining -= widths[i];
				weights -= stretch;
			} else if (used) {
				const auto requested = widths[i];
				widths[i] = int(qint64(reserved) * requested / used);
				reserved -= widths[i];
				used -= requested;
			}
		}
		for (auto i = 0; i != count; ++i) {
			resizeChild(children[i].widget.data(), widths[i]);
		}
		return placeLine(children, 0, count, newWidth, 0, spacing);
	}

private:
	struct Child {
		QPointer<Ui::RpWidget> widget;
		Size size;
	};

	void resizeChild(not_null<Ui::RpWidget*> widget, int width) {
		if (const auto button = dynamic_cast<Ui::RoundButton*>(widget.get())) {
			button->setFullWidth(std::max(width, 1));
		} else {
			const auto margins = widget->getMargins();
			widget->resizeToWidth(std::max(width - margins.left() - margins.right(), 1));
		}
	}

	int placeLine(const std::vector<Child> &children, int first, int last, int width, int top, int spacing) {
		auto height = 0;
		for (auto i = first; i != last; ++i) {
			height = std::max(height, children[i].widget->height());
		}
		auto left = 0;
		for (auto i = first; i != last; ++i) {
			const auto widget = children[i].widget.data();
			const auto shift = (_align & Qt::AlignBottom)
				? (height - widget->height())
				: (_align & Qt::AlignVCenter)
				? (height - widget->height()) / 2
				: 0;
			const auto margins = widget->getMargins();
			widget->moveToLeft(left + margins.left(), top + shift + margins.top(), width);
			left += widget->width() + spacing;
		}
		return height;
	}

	int layoutWrapped(const std::vector<Child> &children, const std::vector<int> &desired, int width) {
		auto top = 0;
		auto lineStart = 0;
		auto used = 0;
		const auto count = int(desired.size());
		for (auto i = 0; i != count; ++i) {
			const auto childWidth = std::clamp(desired[i], 1, width);
			if (i != lineStart && used + _spacing + childWidth > width) {
				top += placeLine(children, lineStart, i, width, top, _spacing) + _spacing;
				lineStart = i;
				used = 0;
			}
			resizeChild(children[i].widget.data(), childWidth);
			used += (i == lineStart ? 0 : _spacing) + children[i].widget->width();
		}
		return top + placeLine(children, lineStart, count, width, top, _spacing);
	}


	[[nodiscard]] static int natural(not_null<Ui::RpWidget*> widget) {
		const auto margins = widget->getMargins();
		if (const auto label = dynamic_cast<Ui::FlatLabel*>(widget.get())) {
			return label->textMaxWidth() + margins.left() + margins.right();
		} else if (const auto button = dynamic_cast<Ui::RoundButton*>(widget.get())) {
			button->setFullWidth(0);
			return button->width();
		}
		const auto result = widget->naturalWidth();
		return (result > 0)
			? result + margins.left() + margins.right()
			: std::max(widget->sizeHint().width(), 0);
	}

	const Qt::Alignment _align;
	const int _spacing = 0;
	const bool _wrap = false;
	std::vector<Child> _children;
	bool _resizing = false;

};

} // namespace

QJsonObject PluginStylePalette() {
	auto buttons = QJsonObject();
	const auto hex = [](const QColor &color) {
		return color.name(QColor::HexRgb).toUpper();
	};
	const auto settingsColor = [&](const style::color &color) {
		return hex(color->c);
	};
	const auto dp = [](int scaled) {
		return qRound(scaled * 100. / style::Scale());
	};
	const auto add = [&](QString id, const ButtonOverrides &colors, int radius) {
		auto fields = QJsonObject();
		const auto insert = [&](QString key, const std::optional<QColor> &color) {
			fields.insert(key, color && color->alpha() ? QJsonValue(hex(*color))
				: QJsonValue(QJsonValue::Null));
		};
		insert(u"text"_q, colors.text);
		insert(u"background"_q, colors.background);
		insert(u"border"_q, colors.border);
		insert(u"hover_text"_q, colors.hoverText);
		insert(u"hover_background"_q, colors.hoverBackground);
		insert(u"ripple"_q, colors.ripple);
		buttons.insert(id, QJsonObject{ { u"colors"_q, fields }, { u"radius"_q, radius } });
	};

	const auto variants = {
		u"primary"_q, u"secondary"_q, u"danger"_q, u"code"_q, u"chip"_q,
		u"install"_q, u"subtle"_q, u"install_secondary"_q, u"proxy_selector"_q, u"proxy_reset"_q,
		u"dialog"_q, u"dialog_danger"_q, u"native_primary"_q, u"cancel"_q,
		u"link"_q, u"settings_row"_q, u"save_settings"_q, u"settings_link"_q,
		u"file_details"_q, u"card_title"_q, u"card_action"_q, u"card_danger"_q,
		u"menu_action"_q, u"menu_danger"_q, u"popup_item"_q,
		u"choice"_q, u"selected_choice"_q, u"trash"_q, u"close"_q, u"back"_q, u"search_clear"_q,
	};
	for (const auto &variant : variants) {
		if (variant == u"search_clear"_q) {
			const auto &s = st::settingsPluginSearchClear;
			add(variant, {
				.text = s.crossFg->c,
				.hoverText = s.crossFgOver->c,
				.ripple = s.ripple.color->c,
			}, dp(s.cross.size / 2));
		} else if (const auto s = IconStyle(variant)) {
			const auto trash = variant == u"trash"_q;
			auto colors = trash && DarkButtonPalette() ? ReferenceColors(variant) : ButtonOverrides();
			if (!colors.text) colors.text = trash ? st::settingsPluginLabelAttention->c : st::boxTitleCloseFg->c;
			colors.hoverText = trash ? colors.text : std::optional(st::boxTitleCloseFgOver->c);
			colors.ripple = s->ripple.color->c;
			add(variant, colors, dp(trash ? st::settingsPluginRowRadius : s->rippleAreaSize / 2));
		} else if (variant == u"choice"_q || variant == u"selected_choice"_q) {
			const auto selected = variant == u"selected_choice"_q;
			const auto &s = st::settingsPluginChoice;
			const auto text = (selected ? s.textFgOver : s.textFg)->c;
			add(variant, {
				.text = text,
				.background = (selected ? st::settingsPluginChoiceSelected : st::settingsPluginRowHover)->c,
				.hoverText = text,
				.hoverBackground = (selected ? st::settingsPluginChoiceSelected : st::settingsPluginRowPressed)->c,
				.ripple = s.ripple.color->c,
			}, dp(st::settingsPluginRowRadius));
		} else if (const auto s = RowStyle(variant)) {
			const auto link = variant == u"settings_link"_q;
			const auto details = variant == u"file_details"_q;
			const auto card = variant.startsWith(u"card_"_q);
			add(variant, {
				.text = s->textFg->c,
				.background = (link ? st::settingsPluginRowHover : s->textBg)->c,
				.hoverText = s->textFgOver->c,
				.hoverBackground = (link || details ? st::settingsPluginRowPressed
					: card ? st::settingsPluginCardActionOverlay : s->textBgOver)->c,
				.ripple = s->ripple.color->c,
			}, link || details ? dp(st::settingsPluginCardRadius)
				: card ? st::settingsPluginLegacyCardActionRadius : 0);
		} else {
			const auto &style = ButtonStyle(variant);
			auto colors = RoundColors(variant, true);
			colors.hoverText = colors.text.value_or(style.textFgOver->c);
			colors.hoverBackground = colors.background
				? FilledButtonHoverColor(*colors.background, colors.text.value_or(style.textFg->c))
				: style.textBgOver->c;
			colors.text = colors.text.value_or(style.textFg->c);
			colors.background = colors.background.value_or(style.textBg->c);
			colors.ripple = colors.ripple.value_or(style.ripple.color->c);
			add(variant, colors, dp(variant == u"chip"_q ? style.height / 2 : style.radius));
		}
	}
	return {
		{ u"theme"_q, DarkButtonPalette() ? u"dark"_q : u"light"_q },
		{ u"background"_q, settingsColor(st::windowBg) },
		{ u"buttons"_q, buttons },
		{ u"labels"_q, QJsonObject{
			{ u"normal"_q, settingsColor(st::boxLabel.textFg) },
			{ u"muted"_q, settingsColor(st::settingsPluginLabelMuted) },
			{ u"accent"_q, settingsColor(st::settingsPluginLabelAccent) },
			{ u"positive"_q, settingsColor(st::settingsPluginLabelPositive) },
			{ u"attention"_q, settingsColor(st::settingsPluginLabelAttention) },
		} },
	};
}

object_ptr<Ui::RpWidget> PluginUiTree::build(
		QWidget *parent,
		const QJsonObject &node,
		const Event &event,
		int depth) {
	if (depth > kMaximumDepth || ++_count > kMaximumNodes) {
		throw std::runtime_error("UI tree is too deep or too large.");
	}
	const auto type = node.value(u"type"_q).toString();
	const auto id = node.value(u"id"_q).toString();
	const auto text = node.value(u"text"_q).toString();
	const auto sender = [=](const QString &kind, QJsonValue value) {
		event(id, kind, std::move(value));
	};
	auto result = object_ptr<Ui::RpWidget>(nullptr);
	if (type == u"column"_q || type == u"card"_q) {
		auto column = object_ptr<Ui::VerticalLayout>(parent);
		const auto gap = node.contains(u"gap"_q)
			? style::ConvertScale(std::clamp(node.value(u"gap"_q).toInt(), 0, 48))
			: 2 * kColumnSkip;
		const auto margins = QMargins(0, gap / 2, 0, (gap + 1) / 2);
		for (const auto &value : node.value(u"children"_q).toArray()) {
			const auto child = value.toObject();
			auto widget = build(column.data(), child, event, depth + 1);
			if (child.contains(u"visible"_q)) {
				const auto wrap = column->add(
					object_ptr<Ui::SlideWrap<>>(column.data(), std::move(widget), margins));
				wrap->toggle(child.value(u"visible"_q).toBool(), anim::type::instant);
			} else {
				column->add(std::move(widget), margins);
			}
		}
		if (type == u"card"_q) {
			auto card = object_ptr<ContentFrame>(parent, std::move(column), style::ConvertScale(19), 0, true);
			card->setBackground(ReadColor(node.value(u"background"_q)));
			result = std::move(card);
		} else {
			result = std::move(column);
		}
	} else if (type == u"progress"_q) {
		auto meter = object_ptr<Meter>(parent);
		meter->setValue(node.value(u"value"_q).toDouble());
		result = std::move(meter);
	} else if (type == u"divider"_q) {
		auto line = object_ptr<Ui::RpWidget>(parent);
		const auto raw = line.data();
		raw->resize(1, st::lineWidth);
		raw->paintRequest() | rpl::on_next([=](QRect rect) {
			QPainter(raw).fillRect(rect, st::shadowFg);
		}, raw->lifetime());
		result = std::move(line);
	} else if (type == u"row"_q) {
		const auto align = node.value(u"align"_q).toString(u"center"_q);
		auto row = object_ptr<Row>(
			parent,
			(align == u"top"_q) ? Qt::AlignTop
				: (align == u"bottom"_q) ? Qt::AlignBottom
				: (align == u"center"_q) ? Qt::AlignVCenter
				: throw std::runtime_error("Row align must be top, center or bottom."),
			std::clamp(node.value(u"spacing"_q).toInt(kRowSkip), 0, 200),
			node.value(u"wrap"_q).toBool());
		for (const auto &value : node.value(u"children"_q).toArray()) {
			const auto child = value.toObject();
			auto size = Row::Size();
			const auto width = child.value(u"width"_q);
			if (width.isString()) {
				if (width.toString() != u"fit"_q) {
					throw std::runtime_error("Width must be a number or \"fit\".");
				}
				size.fit = true;
			} else if (width.isDouble()) {
				size.fixed = std::clamp(width.toInt(), 0, 4000);
			}
			if (child.contains(u"stretch"_q)) {
				size.stretch = std::clamp(child.value(u"stretch"_q).toInt(), 0, 100);
			}
			row->add(build(row.data(), child, event, depth + 1), size,
				child.value(u"visible"_q).toBool(true));
		}
		result = std::move(row);
	} else if (type == u"text"_q) {
		const auto align = node.value(u"align"_q).toString(u"left"_q);
		const auto bold = node.value(u"bold"_q).toBool();
		auto label = object_ptr<PaletteLabel>(parent, text, LabelStyle(
			(align == u"center"_q) ? style::al_top
				: (align == u"right"_q) ? style::al_topright
				: (align == u"left"_q) ? style::al_topleft
				: throw std::runtime_error("Text align must be left, center or right."), bold));
		label->setTone(node.value(u"tone"_q).toString());
		label->setColor(ReadColor(node.value(u"color"_q)));
		result = std::move(label);
	} else if (type == u"button"_q) {
		const auto variant = node.value(u"variant"_q).toString(u"primary"_q);
		const auto useOvygramPalette = node.value(u"palette"_q).toString() == u"ovygram"_q;
		if (variant == u"search_clear"_q) {
			auto button = object_ptr<PaletteSearchClear>(parent);
			button->setAccessibleName(text);
			button->setClickedCallback([=] { sender(u"click"_q, QJsonValue()); });
			result = std::move(button);
		} else if (IconStyle(variant)) {
			auto button = object_ptr<PaletteIconButton>(parent, variant, useOvygramPalette);
			button->setAccessibleName(text);
			button->setClickedCallback([=] { sender(u"click"_q, QJsonValue()); });
			result = std::move(button);
		} else {
			const auto value = std::make_shared<rpl::variable<QString>>(text);
			_texts[id] = value;
			if (variant == u"choice"_q) {
				auto button = object_ptr<ChoiceButton>(parent, value->value());
				button->setDetail(node.value(u"detail"_q).toString());
				button->setSelected(node.value(u"selected"_q).toBool());
				button->setClickedCallback([=] { sender(u"click"_q, QJsonValue()); });
				result = std::move(button);
			} else if (variant == u"link"_q) {
				auto button = object_ptr<PaletteSettingsButton>(parent, value->value(), *RowStyle(variant), variant);
				const auto raw = button.data();
				const auto icon = Ui::CreateChild<Ui::RpWidget>(raw);
				const auto size = st::settingsPluginSdkLinkArrowSize;
				icon->resize(size, size);
				icon->setAttribute(Qt::WA_TransparentForMouseEvents);
				icon->paintRequest() | rpl::on_next([=](QRect) {
					auto p = QPainter(icon);
					p.setRenderHint(QPainter::Antialiasing);
					auto pen = QPen(st::settingsPluginLabelAccent->c, style::ConvertScale(2));
					pen.setCapStyle(Qt::RoundCap);
					p.setPen(pen);
					const auto from = style::RightToLeft() ? .65 : .35;
					const auto to = 1. - from;
					p.drawLine(QPointF(size * from, size * .22), QPointF(size * to, size * .50));
					p.drawLine(QPointF(size * to, size * .50), QPointF(size * from, size * .78));
				}, icon->lifetime());
				raw->sizeValue() | rpl::on_next([=](QSize outer) {
					const auto inset = st::settingsPluginSdkLinkArrowInset;
					icon->moveToRight(inset, (outer.height() - size) / 2);
					icon->setVisible(outer.width() >= inset + size);
				}, icon->lifetime());
				button->setClickedCallback([=] { sender(u"click"_q, QJsonValue()); });
				result = std::move(button);
			} else if (const auto rowStyle = RowStyle(variant)) {
				const auto fit = node.value(u"width"_q).toString() == u"fit"_q;
				auto button = object_ptr<PaletteSettingsButton>(parent, value->value(), *rowStyle, variant, fit);
				button->setClickedCallback([=] { sender(u"click"_q, QJsonValue()); });
				result = std::move(button);
			} else {
				auto button = object_ptr<PaletteRoundButton>(parent, value->value(), variant, useOvygramPalette);
				button->setClickedCallback([=] { sender(u"click"_q, QJsonValue()); });
				result = std::move(button);
			}
		}
		auto &button = dynamic_cast<ColorableButton&>(*result.data());
		button.setColors(ReadButtonOverrides(node.value(u"colors"_q)));
		button.setRadius(ReadButtonRadius(node.value(u"radius"_q)));
	} else if (type == u"slider"_q) {
		const auto minimum = node.value(u"minimum"_q).toInt();
		const auto maximum = node.value(u"maximum"_q).toInt(255);
		if (minimum >= maximum || minimum < -1000000 || maximum > 1000000) {
			throw std::runtime_error("Invalid slider range.");
		}
		auto slider = object_ptr<PluginSlider>(parent, minimum, maximum,
			[=](int value) { sender(u"slide"_q, value); });
		slider->setAccessibleName(node.value(u"label"_q).toString());
		slider->setPluginValue(node.value(u"value"_q).toInt());
		result = std::move(slider);
	} else if (type == u"input"_q) {
		const auto placeholder = node.value(u"placeholder"_q).toString();
		auto field = node.value(u"search"_q).toBool()
			? object_ptr<Ui::InputField>(object_ptr<SearchField>(parent, placeholder, text))
			: object_ptr<Ui::InputField>(
			parent,
			st::defaultInputField,
			Ui::InputField::Mode::SingleLine,
			rpl::single(placeholder),
			TextWithTags{ text });
		const auto raw = field.data();
		raw->changes() | rpl::on_next([=] {
			sender(u"change"_q, raw->getLastText());
		}, raw->lifetime());
		result = std::move(field);
	} else if (type == u"switch"_q) {
		auto box = object_ptr<Ui::Checkbox>(
			parent,
			text,
			node.value(u"value"_q).toBool());
		box->checkedChanges() | rpl::on_next([=](bool checked) {
			sender(u"toggle"_q, checked);
		}, box->lifetime());
		result = std::move(box);
	} else if (type == u"canvas"_q) {
		auto canvas = object_ptr<Canvas>(
			parent,
			node.value(u"height"_q).toInt(),
			[=](int x, int y) { sender(u"click"_q, QJsonArray{ x, y }); });
		canvas->setCommands(node.value(u"commands"_q).toArray());
		result = std::move(canvas);
	} else if (type == u"image"_q) {
		auto image = QImage(node.value(u"path"_q).toString());
		if (image.isNull()) {
			throw std::runtime_error("Image can't be loaded.");
		}
		result = object_ptr<Picture>(
			parent,
			std::move(image),
			node.value(u"height"_q).toInt());
	} else if (type == u"spacer"_q) {
		auto spacer = object_ptr<Ui::RpWidget>(parent);
		spacer->resize(1, std::clamp(node.value(u"height"_q).toInt(), 0, 1000));
		result = std::move(spacer);
	} else {
		throw std::runtime_error("Unknown UI node type.");
	}
	if (id.isEmpty() || _nodes.contains(id)) {
		throw std::runtime_error("Invalid or repeated node ID.");
	}
	if (node.contains(u"enabled"_q)) result->setEnabled(node.value(u"enabled"_q).toBool());
	if (node.contains(u"tooltip"_q)) result->setToolTip(node.value(u"tooltip"_q).toString());
	_nodes.emplace(id, result.data());
	return result;
}

void PluginUiTree::update(
		const QString &nodeId,
		const QJsonObject &props,
		bool relayoutParent) {
	const auto node = _nodes.find(nodeId);
	if (node == end(_nodes) || !node->second) {
		throw std::runtime_error("Unknown UI node.");
	}
	const auto widget = node->second.data();
	if (props.contains(u"text"_q)) {
		const auto text = props.value(u"text"_q).toString();
		if (const auto label = dynamic_cast<Ui::FlatLabel*>(widget)) {
			label->setText(text);
		} else if (const auto field = dynamic_cast<Ui::InputField*>(widget)) {
			field->setText(text);
		} else if (const auto value = _texts.find(nodeId);
				value != end(_texts)) {
			*value->second = text;
			if (const auto button = dynamic_cast<PaletteSettingsButton*>(widget)) {
				button->refreshNaturalWidth();
			}
		}
	}
	if (props.contains(u"enabled"_q)) widget->setEnabled(props.value(u"enabled"_q).toBool());
	if (props.contains(u"tooltip"_q)) widget->setToolTip(props.value(u"tooltip"_q).toString());
	if (const auto button = dynamic_cast<ChoiceButton*>(widget)) {
		if (props.contains(u"detail"_q)) button->setDetail(props.value(u"detail"_q).toString());
		if (props.contains(u"selected"_q)) button->setSelected(props.value(u"selected"_q).toBool());
	}
	if (const auto label = dynamic_cast<PaletteLabel*>(widget)) {
		if (props.contains(u"tone"_q)) label->setTone(props.value(u"tone"_q).toString());
		if (props.contains(u"color"_q)) label->setColor(ReadColor(props.value(u"color"_q)));
	}
	if (const auto button = dynamic_cast<ColorableButton*>(widget)) {
		if (props.contains(u"colors"_q)) {
			button->setColors(ReadButtonOverrides(props.value(u"colors"_q)));
		}
		if (props.contains(u"radius"_q)) {
			button->setRadius(ReadButtonRadius(props.value(u"radius"_q)));
		}
	}
	if (props.contains(u"background"_q)) {
		if (const auto card = dynamic_cast<ContentFrame*>(widget)) {
			card->setBackground(ReadColor(props.value(u"background"_q)));
		}
	}
	if (props.contains(u"value"_q)) {
		if (const auto slider = dynamic_cast<PluginSlider*>(widget)) {
			slider->setPluginValue(props.value(u"value"_q).toInt());
		}
		if (const auto meter = dynamic_cast<Meter*>(widget)) {
			meter->setValue(props.value(u"value"_q).toDouble());
		}
		if (const auto box = dynamic_cast<Ui::Checkbox*>(widget)) {
			box->setChecked(
				props.value(u"value"_q).toBool(),
				Ui::Checkbox::NotifyAboutChange::DontNotify);
		}
	}
	if (props.contains(u"commands"_q)) {
		if (const auto canvas = dynamic_cast<Canvas*>(widget)) {
			canvas->setCommands(props.value(u"commands"_q).toArray());
		}
	}
	// An overlay's parent is the Telegram widget it covers: not ours to lay
	// out, and its size doesn't depend on the overlay.
	const auto parent = relayoutParent ? widget->parentWidget() : nullptr;
	if (props.contains(u"visible"_q)) {
		const auto visible = props.value(u"visible"_q).toBool();
		const auto wrapper = parent ? parent->parentWidget() : nullptr;
		if (const auto wrap = dynamic_cast<Ui::SlideWrap<>*>(wrapper)) {
			wrap->toggle(visible, anim::type::instant);
		} else if (dynamic_cast<Row*>(parent)) {
			widget->setVisible(visible);
		}
	}
	if (const auto rp = dynamic_cast<Ui::RpWidget*>(parent);
			rp && !dynamic_cast<Ui::BoxLayerWidget*>(rp)
			&& (props.contains(u"text"_q) || props.contains(u"visible"_q))) {
		// resizeToWidth expects the content width and adds the margins itself.
		rp->resizeToWidth(std::max(rp->widthNoMargins(), 0));
	}
}

void PluginUiTree::AddDialogButtons(
		not_null<Ui::GenericBox*> box,
		object_ptr<Ui::RpWidget> footerAction) {
	const auto action = dynamic_cast<Ui::AbstractButton*>(footerAction.data());
	if (action) {
		footerAction.release();
		box->addButton(object_ptr<Ui::AbstractButton>::fromRaw(action));
	}
	box->addButton(rpl::single(action ? u"Отмена"_q : u"Закрыть"_q), [=] {
		box->closeBox();
	}, st::settingsPluginDialog);
}

object_ptr<Ui::RpWidget> PluginUiTree::CenterContent(
		QWidget *parent,
		object_ptr<Ui::RpWidget> content,
		int padding,
		int cap) {
	return object_ptr<ContentFrame>(parent, std::move(content), padding,
		style::ConvertScale(std::clamp(cap, 200, 1500)), false);
}

void PluginUiTree::ResizeCanvas(not_null<Ui::RpWidget*> widget, QSize size) {
	const auto canvas = static_cast<Canvas*>(widget.get());
	canvas->setHeight(size.height());
	canvas->resize(size);
	canvas->raise();
}

} // namespace Ovygram
