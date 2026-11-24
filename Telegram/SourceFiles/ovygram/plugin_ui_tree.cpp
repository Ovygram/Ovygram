#include "ovygram/plugin_ui_tree.h"

#include "ovygram/plugin_paint.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "styles/style_layers.h"
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
[[nodiscard]] const style::FlatLabel &LabelStyle(style::align align) {
	if (align == style::al_topleft) {
		return st::boxLabel;
	}
	static auto center = std::optional<style::FlatLabel>();
	static auto right = std::optional<style::FlatLabel>();
	auto &result = (align == style::al_top) ? center : right;
	if (!result) {
		result.emplace(st::boxLabel);
		result->align = align;
		result->minWidth = 0;
	}
	return *result;
}

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

	Row(QWidget *parent, Qt::Alignment align, int spacing)
	: RpWidget(parent)
	, _align(align)
	, _spacing(spacing) {
	}

	void add(object_ptr<Ui::RpWidget> child, Size size) {
		const auto raw = child.release();
		raw->setParent(this);
		raw->show();
		_children.push_back({ raw, size });
		raw->heightValue(
		) | rpl::skip(1) | rpl::on_next([this] {
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
		const auto count = int(_children.size());
		if (!count) {
			return 0;
		}
		_resizing = true;
		const auto available = std::max(newWidth - _spacing * (count - 1), 0);
		auto widths = std::vector<int>(count, -1);
		auto used = 0;
		auto weights = 0;
		for (auto i = 0; i != count; ++i) {
			const auto &[widget, size] = _children[i];
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
		for (auto i = 0; i != count; ++i) {
			if (widths[i] < 0) {
				const auto stretch = _children[i].size.stretch;
				widths[i] = weights ? (remaining * stretch / weights) : 0;
				remaining -= widths[i];
				weights -= stretch;
			}
		}
		auto height = 0;
		for (auto i = 0; i != count; ++i) {
			const auto &[widget, size] = _children[i];
			const auto button = dynamic_cast<Ui::RoundButton*>(widget.data());
			if (button) {
				// Without an override a button keeps its natural width.
				button->setFullWidth(size.fit ? 0 : std::max(widths[i], 1));
			} else {
				widget->resizeToWidth(std::max(widths[i], 1));
			}
			height = std::max(height, widget->height());
		}
		auto left = 0;
		for (auto i = 0; i != count; ++i) {
			const auto widget = _children[i].widget.data();
			const auto top = (_align & Qt::AlignBottom)
				? (height - widget->height())
				: (_align & Qt::AlignVCenter)
				? (height - widget->height()) / 2
				: 0;
			widget->moveToLeft(left, top, newWidth);
			left += widget->width() + _spacing;
		}
		_resizing = false;
		return height;
	}

private:
	struct Child {
		QPointer<Ui::RpWidget> widget;
		Size size;
	};

	[[nodiscard]] static int natural(not_null<Ui::RpWidget*> widget) {
		if (const auto label = dynamic_cast<Ui::FlatLabel*>(widget.get())) {
			return label->textMaxWidth();
		} else if (dynamic_cast<Ui::RoundButton*>(widget.get())) {
			return widget->width();
		}
		const auto result = widget->naturalWidth();
		return (result > 0) ? result : widget->sizeHint().width();
	}

	const Qt::Alignment _align;
	const int _spacing = 0;
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
	if (type == u"column"_q) {
		auto column = object_ptr<Ui::VerticalLayout>(parent);
		for (const auto &child : node.value(u"children"_q).toArray()) {
			column->add(
				build(column.data(), child.toObject(), event, depth + 1),
				QMargins(0, kColumnSkip, 0, kColumnSkip));
		}
		result = std::move(column);
	} else if (type == u"row"_q) {
		const auto align = node.value(u"align"_q).toString(u"center"_q);
		auto row = object_ptr<Row>(
			parent,
			(align == u"top"_q) ? Qt::AlignTop
				: (align == u"bottom"_q) ? Qt::AlignBottom
				: (align == u"center"_q) ? Qt::AlignVCenter
				: throw std::runtime_error("Row align must be top, center or bottom."),
			std::clamp(node.value(u"spacing"_q).toInt(kRowSkip), 0, 200));
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
			row->add(build(row.data(), child, event, depth + 1), size);
		}
		result = std::move(row);
	} else if (type == u"text"_q) {
		const auto align = node.value(u"align"_q).toString(u"left"_q);
		result = object_ptr<Ui::FlatLabel>(
			parent,
			text,
			(align == u"center"_q) ? LabelStyle(style::al_top)
				: (align == u"right"_q) ? LabelStyle(style::al_topright)
				: (align == u"left"_q) ? LabelStyle(style::al_topleft)
				: throw std::runtime_error("Text align must be left, center or right."));
	} else if (type == u"button"_q) {
		const auto value = std::make_shared<rpl::variable<QString>>(text);
		_texts[id] = value;
		auto button = object_ptr<Ui::RoundButton>(
			parent,
			value->value(),
			st::defaultActiveButton);
		button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
		button->setClickedCallback([=] { sender(u"click"_q, QJsonValue()); });
		result = std::move(button);
	} else if (type == u"input"_q) {
		auto field = object_ptr<Ui::InputField>(
			parent,
			st::defaultInputField,
			Ui::InputField::Mode::SingleLine,
			rpl::single(node.value(u"placeholder"_q).toString()),
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
	if (props.contains(u"value"_q)) {
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
	if (const auto rp = dynamic_cast<Ui::RpWidget*>(parent)) {
		rp->resizeToWidth(rp->width());
	}
}

void PluginUiTree::ResizeCanvas(not_null<Ui::RpWidget*> widget, QSize size) {
	const auto canvas = static_cast<Canvas*>(widget.get());
	canvas->setHeight(size.height());
	canvas->resize(size);
	canvas->raise();
}

} // namespace Ovygram
