#include "ovygram/plugin_ui_tree.h"

#include "ovygram/plugin_paint.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QJsonArray>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>

#include <algorithm>
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

void SetLabelTone(not_null<Ui::FlatLabel*> label, const QString &tone) {
	if (tone == u"muted"_q) label->setTextColorOverride(st::windowSubTextFg->c);
	else if (tone == u"accent"_q) label->setTextColorOverride(st::windowActiveTextFg->c);
	else if (tone == u"positive"_q) label->setTextColorOverride(st::groupCallLive1->c);
	else if (tone == u"attention"_q) label->setTextColorOverride(st::attentionButtonFg->c);
	else label->setTextColorOverride(std::nullopt);
}

[[nodiscard]] const style::RoundButton &ButtonStyle(const QString &variant) {
	if (variant == u"primary"_q) return st::defaultActiveButton;
	if (variant == u"chip"_q) {
		static const auto chip = [] {
			auto style = st::defaultLightButton;
			style.height = style::ConvertScale(24);
			style.width = -style::ConvertScale(18);
			style.textTop = std::max((style.height - style.style.font->height) / 2, 0);
			style.textBg = st::windowBgOver;
			style.textFg = st::windowBoldFg;
			return style;
		}();
		return chip;
	}
	if (variant != u"code"_q) return st::defaultLightButton;
	static const auto code = [] {
		auto style = st::defaultLightButton;
		style.style.font = style.style.font->monospace();
		style.height = style::ConvertScale(25);
		style.textTop = std::max((style.height - style.style.font->height) / 2, 0);
		style.textBg = st::windowBg;
		return style;
	}();
	return code;
}

void SetButtonVariant(not_null<Ui::RoundButton*> button, const QString &variant) {
	if (variant == u"chip"_q) button->setFullRadius(true);
	if (variant != u"secondary"_q && variant != u"danger"_q) return;
	const auto apply = [=] {
		if (variant == u"danger"_q) {
			auto tint = st::attentionButtonFg->c;
			button->setTextFgOverride(tint);
			tint.setAlpha(35);
			button->setBrushOverride(QBrush(tint));
		} else {
			auto tint = st::lightButtonFg->c;
			tint.setAlpha(115);
			button->setBrushOverride(QBrush(st::windowBg->c));
			button->setPenOverride(QPen(tint));
			button->setRippleOverride(tint);
		}
	};
	apply();
	style::PaletteChanged() | rpl::on_next(apply, button->lifetime());
}

class ChoiceButton final : public Ui::SettingsButton {
public:
	ChoiceButton(QWidget *parent, rpl::producer<QString> text)
	: SettingsButton(parent, std::move(text), Style()) {
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
		auto hq = PainterHighQualityEnabler(p);
		p.setPen(Qt::NoPen);
		const auto over = isOver() && !isDisabled();
		p.setBrush(_selected ? st::windowBgActive
			: over ? st::windowBgRipple : st::windowBgOver);
		const auto radius = style::ConvertScale(6);
		p.drawRoundedRect(rect(), radius, radius);
		paintRipple(p, 0, 0);
		paintText(p, _selected, width());
		if (!_detail.isEmpty()) {
			const auto font = st().style.font;
			p.setFont(font);
			p.setPen(_selected ? st::windowFgActive : st::windowSubTextFg);
			p.drawTextRight(st().padding.right(), (height() - font->height) / 2,
				width(), _detail);
		}
	}

private:
	static const style::SettingsButton &Style() {
		static const auto result = [] {
			auto style = st::defaultSettingsButton;
			style.style = st::semiboldTextStyle;
			style.textFgOver = st::windowFgActive;
			style.height = std::max(style.style.font->height, style::ConvertScale(18));
			style.padding = QMargins(style::ConvertScale(11), style::ConvertScale(5),
				style::ConvertScale(11), style::ConvertScale(5));
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
		p.setBrush(background);
		p.drawRoundedRect(rect(), style::ConvertScale(12), style::ConvertScale(12));
	}
private:
	const not_null<Ui::RpWidget*> _child;
	const int _padding;
	const int _cap;
	const bool _card;
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
			result = object_ptr<ContentFrame>(parent, std::move(column), style::ConvertScale(19), 0, true);
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
		auto label = object_ptr<Ui::FlatLabel>(parent, text, LabelStyle(
			(align == u"center"_q) ? style::al_top
				: (align == u"right"_q) ? style::al_topright
				: (align == u"left"_q) ? style::al_topleft
				: throw std::runtime_error("Text align must be left, center or right."), bold));
		const auto tone = node.value(u"tone"_q).toString();
		SetLabelTone(label.data(), tone);
		result = std::move(label);
	} else if (type == u"button"_q) {
		const auto variant = node.value(u"variant"_q).toString(u"primary"_q);
		if (variant == u"trash"_q) {
			auto button = object_ptr<Ui::IconButton>(parent, st::boxTitleClose);
			button->setIconOverride(&st::menuIconDeleteAttention, &st::menuIconDeleteAttention);
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
				auto button = object_ptr<Ui::SettingsButton>(parent, value->value());
				const auto raw = button.data();
				const auto icon = Ui::CreateChild<Ui::RpWidget>(raw);
				const auto size = style::ConvertScale(14);
				icon->resize(size, size);
				icon->setAttribute(Qt::WA_TransparentForMouseEvents);
				icon->paintRequest() | rpl::on_next([=](QRect) {
					auto p = QPainter(icon);
					p.setRenderHint(QPainter::Antialiasing);
					auto pen = QPen(st::windowActiveTextFg->c, style::ConvertScale(2));
					pen.setCapStyle(Qt::RoundCap);
					p.setPen(pen);
					const auto from = style::RightToLeft() ? .65 : .35;
					const auto to = 1. - from;
					p.drawLine(QPointF(size * from, size * .22), QPointF(size * to, size * .50));
					p.drawLine(QPointF(size * to, size * .50), QPointF(size * from, size * .78));
				}, icon->lifetime());
				raw->sizeValue() | rpl::on_next([=](QSize outer) {
					icon->moveToRight(style::ConvertScale(12), (outer.height() - size) / 2);
				}, icon->lifetime());
				icon->show();
				button->setClickedCallback([=] { sender(u"click"_q, QJsonValue()); });
				result = std::move(button);
			} else {
				auto button = object_ptr<Ui::RoundButton>(parent, value->value(), ButtonStyle(variant));
				button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
				SetButtonVariant(button.data(), variant);
				button->setClickedCallback([=] { sender(u"click"_q, QJsonValue()); });
				result = std::move(button);
			}
		}
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
		}
	}
	if (props.contains(u"enabled"_q)) widget->setEnabled(props.value(u"enabled"_q).toBool());
	if (props.contains(u"tooltip"_q)) widget->setToolTip(props.value(u"tooltip"_q).toString());
	if (const auto button = dynamic_cast<ChoiceButton*>(widget)) {
		if (props.contains(u"detail"_q)) button->setDetail(props.value(u"detail"_q).toString());
		if (props.contains(u"selected"_q)) button->setSelected(props.value(u"selected"_q).toBool());
	}
	if (props.contains(u"tone"_q)) {
		if (const auto label = dynamic_cast<Ui::FlatLabel*>(widget)) {
			SetLabelTone(label, props.value(u"tone"_q).toString());
		}
	}
	if (props.contains(u"value"_q)) {
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
			rp && (props.contains(u"text"_q) || props.contains(u"visible"_q))) {
		// resizeToWidth expects the content width and adds the margins itself.
		rp->resizeToWidth(std::max(rp->widthNoMargins(), 0));
	}
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
