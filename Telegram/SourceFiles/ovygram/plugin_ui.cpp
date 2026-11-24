#include "ovygram/plugin_ui.h"
#include "window/notifications_manager.h"
#include <QtWidgets/QBoxLayout>
#include "ovygram/protected_ui.h"

#include "ovygram/chat_bars.h"
#include "ovygram/chat_decorations.h"
#include "ovygram/chat_list_filter.h"
#include "ovygram/message_decorations.h"
#include "ovygram/plugin_elements.h"
#include "ovygram/plugin_install.h"
#include "ovygram/plugin_menus.h"
#include "ovygram/plugin_actions.h"
#include "ovygram/plugin_panel_buttons.h"
#include "ovygram/plugin_native.h"
#include "ovygram/plugin_paint.h"
#include "ovygram/plugin_section.h"
#include "ovygram/plugin_search.h"
#include "ovygram/plugin_list_visibility.h"
#include "ovygram/plugin_widgets.h"

#include "base/algorithm.h"
#include "base/unique_qptr.h"
#include "core/application.h"
#include "data/data_peer.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/ui_utility.h"
#include "ui/widgets/separate_panel.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QPointer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Ovygram {
namespace {

constexpr auto kMaximumDepth = 32;
constexpr auto kMaximumNodes = 2000;
constexpr auto kMaximumCanvasHeight = 4096;
constexpr auto kMaximumCanvasOps = 2000;
constexpr auto kRowSkip = 8;
constexpr auto kColumnSkip = 6;
constexpr auto kBarPadding = 6;

PluginUi *GlobalInstance = nullptr;

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

// Keeps an overlay canvas over its target widget.
class OverlayWatcher final : public QObject {
public:
	OverlayWatcher(QWidget *target, Canvas *canvas)
	: QObject(canvas)
	, _canvas(canvas) {
		target->installEventFilter(this);
	}

protected:
	bool eventFilter(QObject *watched, QEvent *e) override {
		if (e->type() == QEvent::Resize && _canvas) {
			const auto size = static_cast<QResizeEvent*>(e)->size();
			_canvas->setHeight(size.height());
			_canvas->resize(size);
			_canvas->raise();
		}
		return false;
	}

private:
	const QPointer<Canvas> _canvas;

};

// Background and padding of a plugin bar inside the chat.
class BarFrame final : public Ui::RpWidget {
public:
	BarFrame(object_ptr<Ui::RpWidget> content)
	: RpWidget(nullptr)
	, _content(content.release()) {
		_content->setParent(this);
		_content->show();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto padding = st::boxRowPadding;
		_content->resizeToWidth(
			std::max(newWidth - padding.left() - padding.right(), 1));
		_content->moveToLeft(padding.left(), kBarPadding, newWidth);
		return _content->height() + 2 * kBarPadding;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		p.fillRect(rect(), st::windowBg);
		p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, st::shadowFg);
	}

private:
	const QPointer<Ui::RpWidget> _content;

};

} // namespace

int LayoutChatBars(QWidget *chat, PeerData *peer, int left, int top, int width) {
	const auto instance = PluginUi::Instance();
	return instance
		? instance->layoutChatBars(chat, peer, left, top, width)
		: 0;
}

struct PluginUi::Object {
	QString installationId;
	QString objectId;
	QPointer<Ui::BoxContent> box;
	QPointer<Ui::RpWidget> inserted;
	std::unique_ptr<Ui::SeparatePanel> panel;
	base::unique_qptr<Ui::RpWidget> bar;
	base::unique_qptr<Ui::RpWidget> overlay;
	base::unique_qptr<Ui::RpWidget> section;
	QString title;
	QString accountId;
	QString chatId;
	std::map<QString, QPointer<QWidget>> nodes;
	std::map<QString, std::shared_ptr<rpl::variable<QString>>> texts;
	int count = 0;
	bool silent = false;
};

PluginUi::PluginUi(EventSender send)
: _send(std::move(send)) {
	GlobalInstance = this;
	Elements::SetEventSender(_send);
	Widgets::SetEventSender(_send);
	Menus::SetEventSender(_send);
	Actions::SetEventSender(_send);
	PanelButtons::SetEventSender(_send);
	Search::SetEventSender(_send);
	Native::SetEventSender(_send);
	// A canvas over a widget that contains a protected surface would cover
	// it: such overlays stay hidden while the surface exists. A dialog of a
	// plugin is a layer above everything, so it is closed.
	ProtectedSurfacesChanged() | rpl::on_next([=] {
		PanelButtons::UpdateProtectedSurfaces();
		auto boxes = std::vector<QPointer<Ui::BoxContent>>();
		for (const auto &[id, object] : _objects) {
			if (const auto row = object->inserted.data()) {
				row->setVisible(!ProtectedFromPlugins(row->parentWidget())
					&& !CouldCoverProtected(row, u"geometry"_q));
			}
			if (const auto overlay = object->overlay.get()) {
				overlay->setVisible(
					!ProtectedFromPlugins(overlay->parentWidget()));
			}
			if (object->box) {
				boxes.push_back(object->box);
			}
		}
		if (ProtectedSurfaceExists()) {
			// Closing a box removes its object from the map.
			for (const auto &box : boxes) {
				if (box) {
					box->closeBox();
				}
			}
		}
	}, _lifetime);
}

PluginUi *PluginUi::Instance() {
	return GlobalInstance;
}

PluginUi::~PluginUi() {
	if (GlobalInstance == this) {
		GlobalInstance = nullptr;
		Elements::SetEventSender(nullptr);
		Widgets::SetEventSender(nullptr);
		Menus::SetEventSender(nullptr);
		Actions::SetEventSender(nullptr);
		PanelButtons::SetEventSender(nullptr);
		Search::SetEventSender(nullptr);
		Native::SetEventSender(nullptr);
	}
	// Surfaces report their destruction to destroyed(), which erases from
	// _objects. Disconnect them first, so destroying _objects can't reenter.
	auto objects = base::take(_objects);
	for (const auto &[id, object] : objects) {
		object->silent = true;
		delete object->inserted.data();
		if (object->section) {
			_sectionClosed.fire_copy(id);
		}
		for (const auto widget : std::initializer_list<QObject*>{
				object->bar.get(),
				object->overlay.get(),
				object->section.get(),
				object->box.data() }) {
			if (widget) {
				QObject::disconnect(widget, nullptr, this, nullptr);
			}
		}
		if (object->box) {
			// The box outlives us during its closing animation; its widgets
			// check the guard in build() before sending events.
			object->box->closeBox();
		}
	}
}

void PluginUi::handle(const QJsonObject &command) {
	const auto method = command.value(u"method"_q).toString();
	if (method == u"ui.close"_q || method == u"ui.update"_q) {
		const auto i = _objects.find(command.value(u"object_id"_q).toString());
		if (i != end(_objects) && i->second->installationId != command.value(u"installation_id"_q).toString()) {
			throw std::runtime_error("Surface belongs to another installation.");
		}
	}
	if (method == u"ui.show"_q) {
		show(command);
	} else if (method == u"ui.update"_q) {
		update(command);
	} else if (method == u"ui.close"_q) {
		close(command.value(u"object_id"_q).toString(), true);
	} else if (method == u"ui.close_plugin"_q) {
		closePlugin(command.value(u"installation_id"_q).toString());
		Elements::RemovePlugin(command.value(u"installation_id"_q).toString());
		Widgets::RemovePlugin(command.value(u"installation_id"_q).toString());
		Decorations::RemovePlugin(command.value(u"installation_id"_q).toString());
		Search::RemovePlugin(command.value(u"installation_id"_q).toString());
		ListVisibility::RemovePlugin(command.value(u"installation_id"_q).toString());
		ChatListFilter::RemovePlugin(command.value(u"installation_id"_q).toString());
		MessageDecorations::RemovePlugin(command.value(u"installation_id"_q).toString());
		Actions::RemovePlugin(command.value(u"installation_id"_q).toString());
		Menus::RemovePlugin(command.value(u"installation_id"_q).toString());
		PanelButtons::RemovePlugin(command.value(u"installation_id"_q).toString());
		Window::Notifications::RemovePluginNotificationRules(command.value(u"installation_id"_q).toString());
		Native::RemovePlugin(command.value(u"installation_id"_q).toString());
	} else if (method.startsWith(u"ui.list_visibility."_q)) {
		ListVisibility::Handle(command);
	} else if (method.startsWith(u"ui.search."_q)) {
		Search::Handle(command);
	} else if (method == u"ui.action.remove"_q) {
		Actions::Remove(command.value(u"installation_id"_q).toString(), command.value(u"rule_id"_q).toString());
	} else if (method == u"ui.notification.add"_q || method == u"ui.notification.remove"_q) {
		Window::Notifications::SetPluginNotificationRule(command);
	} else if (method.startsWith(u"ui.menu."_q)) {
		Menus::Handle(command);
	} else if (method.startsWith(u"ui.panel."_q)) {
		PanelButtons::Handle(command);
	} else if (method.startsWith(u"ui.native."_q)) {
		Native::Handle(command);
	} else if (method.startsWith(u"ui.message_decoration."_q)) {
		MessageDecorations::Handle(command);
	} else if (method.startsWith(u"ui.chat_list."_q)) {
		ChatListFilter::Handle(command);
	} else if (method.startsWith(u"ui.decoration."_q)) {
		Decorations::Handle(command);
	} else if (method.startsWith(u"ui.element."_q)
		|| method.startsWith(u"ui.compose."_q)
		|| method == u"ui.palette"_q) {
		Elements::Handle(command);
	} else if (method.startsWith(u"ui.widgets."_q) || method.startsWith(u"ui.keys."_q)) {
		Widgets::Handle(command);
	} else if (method == u"ui.clipboard"_q) {
		QGuiApplication::clipboard()->setText(command.value(u"text"_q).toString());
	} else {
		throw std::runtime_error("Unknown UI command.");
	}
}

void PluginUi::show(const QJsonObject &command) {
	const auto objectId = command.value(u"object_id"_q).toString();
	if (objectId.isEmpty() || _objects.contains(objectId)) {
		throw std::runtime_error("Invalid or repeated object ID.");
	}
	auto owned = std::make_unique<Object>();
	const auto object = owned.get();
	object->installationId = command.value(u"installation_id"_q).toString();
	object->objectId = objectId;
	const auto title = command.value(u"title"_q).toString();
	const auto tree = command.value(u"tree"_q).toObject();
	const auto kind = command.value(u"kind"_q).toString();
	if (kind == u"dialog"_q) {
		if (ProtectedSurfaceExists()) {
			// It would be drawn over the plugin manager with Telegram's own
			// components and could pass for a window of Ovygram.
			throw std::runtime_error(
				"Dialogs are unavailable while Ovygram's plugin manager is open.");
		}
		const auto window = Core::App().activePrimaryWindow();
		if (!window) {
			throw std::runtime_error("No active Telegram window.");
		}
		const auto width = command.value(u"width"_q).toInt();
		// Built before the box, so an invalid tree throws before showing.
		const auto content = std::make_shared<object_ptr<Ui::RpWidget>>(
			build(*object, nullptr, tree, 0));
		auto box = Box([=](not_null<Ui::GenericBox*> box) {
			box->setTitle(rpl::single(title));
			if (width > 0) {
				const auto available = window->widget()->width() - style::ConvertScale(32);
				box->setWidth(std::min(style::ConvertScale(std::clamp(width, 320, 960)),
					std::max(available, 1)));
			}
			box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); });
			const auto raw = content->data();
			raw->setParent(box);
			box->addRow(std::move(*content), style::al_justify);
			raw->show();
			box->addButton(rpl::single(u"Закрыть"_q), [=] {
				box->closeBox();
			});
			if (const auto i = _objects.find(objectId); i != end(_objects)) {
				i->second->box = box.get();
			}
			connect(box.get(), &QObject::destroyed, this, [=] {
				destroyed(objectId);
			});
		});
		_objects.emplace(objectId, std::move(owned));
		window->uiShow()->showBox(std::move(box));
	} else if (kind == u"window"_q) {
		object->panel = std::make_unique<Ui::SeparatePanel>();
		const auto panel = object->panel.get();
		panel->setWindowFlag(Qt::WindowStaysOnTopHint, false);
		panel->setTitle(rpl::single(title));
		panel->setInnerSize(QSize(
			std::clamp(command.value(u"width"_q).toInt(420), 200, 2000),
			std::clamp(command.value(u"height"_q).toInt(480), 120, 2000)),
			true);
		// Content taller than the window scrolls. ScrollArea isn't an
		// RpWidget, so it fills a plain container that the panel shows.
		auto inner = base::make_unique_q<Ui::RpWidget>(panel);
		const auto scroll = Ui::CreateChild<Ui::ScrollArea>(
			inner.get(),
			st::boxScroll);
		inner->sizeValue() | rpl::on_next([=](QSize size) {
			scroll->setGeometry(QRect(QPoint(), size));
		}, inner->lifetime());
		const auto layout = scroll->setOwnedWidget(
			object_ptr<Ui::VerticalLayout>(scroll));
		auto content = build(*object, layout.data(), tree, 0);
		if (const auto cap = command.value(u"max_content_width"_q).toInt(); cap > 0) {
			content = object_ptr<ContentFrame>(layout, std::move(content), style::ConvertScale(8),
				style::ConvertScale(std::clamp(cap, 200, 1500)), false);
		}
		layout->add(std::move(content), st::boxRowPadding, style::al_justify);
		scroll->widthValue() | rpl::on_next([=](int width) {
			if (layout) {
				layout->resizeToWidth(width);
			}
		}, layout->lifetime());
		panel->showInner(std::move(inner));
		panel->closeRequests(
		) | rpl::on_next([=] {
			// The panel can't be destroyed from its own signal.
			crl::on_main(this, [=] { close(objectId, true); });
		}, panel->lifetime());
		_objects.emplace(objectId, std::move(owned));
		panel->showAndActivate();
	} else if (kind == u"insert"_q) {
		const auto target = Widgets::Resolve(command.value(u"target"_q));
		const auto position = command.value(u"position"_q).toString();
		const auto inside = (position == u"inside"_q);
		if (Core::App().passcodeLocked() || !target || (!inside && position != u"before"_q && position != u"after"_q)) {
			throw std::runtime_error("Invalid insertion target or position.");
		}
		const auto parent = inside ? target : target->parentWidget();
		const auto layout = dynamic_cast<Ui::VerticalLayout*>(parent);
		const auto box = parent ? qobject_cast<QBoxLayout*>(parent->layout()) : nullptr;
		if ((!layout && !box) || ProtectedFromPlugins(parent)
			|| CouldCoverProtected(parent, u"geometry"_q) || InstallationUiActive()) {
			throw std::runtime_error("Target does not accept insertion.");
		}
		const auto ownerRows = std::count_if(_objects.begin(), _objects.end(), [&](const auto &entry) {
			return entry.second->inserted && entry.second->installationId == object->installationId;
		});
		if (ownerRows >= 128) throw std::runtime_error("Too many mounted rows for this installation.");
		const auto count = layout ? layout->count() : box->count();
		auto index = inside ? count : 0;
		if (!inside) {
			if (layout) {
				while (index < count && layout->widgetAt(index).get() != target) ++index;
			} else {
				index = box->indexOf(target);
			}
			if (index < 0 || index == count) throw std::runtime_error("Target is not a layout row.");
			if (position == u"after"_q) ++index;
		}
		auto content = build(*object, parent, tree, 0);
		const auto row = content.data();
		row->setProperty("_ovygram_inserted", true);
		if (layout) {
			layout->insert(index, std::move(content));
		} else {
			box->insertWidget(index, content.release());
		}
		object->inserted = row;
		connect(row, &QObject::destroyed, this, [=] { destroyed(objectId); });
		_objects.emplace(objectId, std::move(owned));
		row->show();
	} else if (kind == u"overlay"_q) {
		const auto target = Widgets::Resolve(command.value(u"target"_q));
		if (!target) {
			throw std::runtime_error("The overlay target no longer exists.");
		} else if (tree.value(u"type"_q).toString() != u"canvas"_q) {
			throw std::runtime_error("An overlay must be a single Canvas.");
		}
		const auto nodeId = tree.value(u"id"_q).toString();
		const auto clickable = command.value(u"clickable"_q).toBool();
		auto canvas = object_ptr<Canvas>(
			target,
			target->height(),
			clickable ? Fn<void(int, int)>([=](int x, int y) {
				if (const auto i = _objects.find(objectId); i != end(_objects)) {
					send(*i->second, nodeId, u"click"_q, QJsonArray{ x, y });
				}
			}) : Fn<void(int, int)>());
		canvas->setCommands(tree.value(u"commands"_q).toArray());
		canvas->setAttribute(Qt::WA_TransparentForMouseEvents, !clickable);
		canvas->setGeometry(target->rect());
		canvas->show();
		canvas->raise();
		new OverlayWatcher(target, canvas.data());
		object->nodes.emplace(nodeId, canvas.data());
		object->overlay = base::unique_qptr<Ui::RpWidget>(canvas.release());
		connect(object->overlay.get(), &QObject::destroyed, this, [=] {
			destroyed(objectId);
		});
		_objects.emplace(objectId, std::move(owned));
	} else if (kind == u"chat_bar"_q) {
		object->accountId = command.value(u"account_id"_q).toString();
		object->chatId = command.value(u"chat_id"_q).toString();
		if (object->accountId.isEmpty() || object->chatId.isEmpty()) {
			throw std::runtime_error("A chat bar needs account_id and chat_id.");
		}
		object->bar = base::unique_qptr<Ui::RpWidget>(
			new BarFrame(build(*object, nullptr, tree, 0)));
		object->bar->hide();
		connect(object->bar.get(), &QObject::destroyed, this, [=] {
			destroyed(objectId);
		});
		_objects.emplace(objectId, std::move(owned));
		relayoutChats();
	} else if (kind == u"section"_q) {
		const auto window = Core::App().activePrimaryWindow();
		const auto controller = window ? window->sessionController() : nullptr;
		if (!controller) {
			throw std::runtime_error("No Telegram window with an account.");
		}
		// Owned here and shown by the section widget, which the window
		// creates and destroys as the user navigates.
		auto layout = base::make_unique_q<Ui::VerticalLayout>(
			static_cast<QWidget*>(nullptr));
		auto content = build(*object, layout.get(), tree, 0);
		if (const auto cap = command.value(u"max_content_width"_q).toInt(); cap > 0) {
			content = object_ptr<ContentFrame>(layout.get(), std::move(content), 0,
				style::ConvertScale(std::clamp(cap, 200, 1500)), false);
		}
		layout->add(std::move(content), st::boxRowPadding, style::al_justify);
		layout->hide();
		object->title = title;
		object->section = base::unique_qptr<Ui::RpWidget>(layout.release());
		connect(object->section.get(), &QObject::destroyed, this, [=] {
			destroyed(objectId);
		});
		_objects.emplace(objectId, std::move(owned));
		ShowPluginSection(controller, objectId);
	} else {
		throw std::runtime_error("Unknown surface kind.");
	}
}

int PluginUi::layoutChatBars(
		QWidget *container,
		PeerData *peer,
		int left,
		int top,
		int width) {
	if (!container) {
		return 0;
	}
	const auto known = std::find_if(begin(_chats), end(_chats), [&](
			const QPointer<QWidget> &chat) {
		return chat.data() == container;
	});
	if (known == end(_chats)) {
		std::erase_if(_chats, [](const QPointer<QWidget> &chat) {
			return !chat;
		});
		_chats.emplace_back(container);
		// A bar lives until the plugin closes it: take it out of a chat
		// that is destroyed. QWidget emits destroyed() before it deletes
		// its children.
		connect(container, &QObject::destroyed, this, [=] {
			for (const auto &[id, object] : _objects) {
				const auto bar = object->bar.get();
				if (bar && bar->parentWidget() == container) {
					bar->hide();
					bar->setParent(nullptr);
				}
			}
		});
	}
	const auto accountId = peer
		? QString::number(peer->session().uniqueId())
		: QString();
	const auto chatId = peer ? QString::number(peer->id.value) : QString();
	auto height = 0;
	for (const auto &[id, object] : _objects) {
		const auto bar = object->bar.get();
		if (!bar) {
			continue;
		} else if (peer
			&& object->accountId == accountId
			&& object->chatId == chatId) {
			if (bar->parentWidget() != container) {
				// On top of the history, but under the panels and menus
				// the chat raises later.
				bar->setParent(container);
				bar->raise();
			}
			bar->resizeToWidth(width);
			bar->moveToLeft(left, top + height, container->width());
			bar->show();
			height += bar->height();
		} else if (bar->parentWidget() == container) {
			bar->hide();
		}
	}
	return height;
}

void PluginUi::relayoutChats() {
	// Deferred: a command can arrive while the chat is still switching
	// peers, and the layout must see the peer it ends up with.
	if (_relayoutQueued) {
		return;
	}
	_relayoutQueued = true;
	crl::on_main(this, [=] {
		_relayoutQueued = false;
		relayoutChatsNow();
	});
}

void PluginUi::relayoutChatsNow() {
	std::erase_if(_chats, [](const QPointer<QWidget> &chat) {
		return !chat;
	});
	for (const auto &chat : _chats) {
		// The chat recomputes its layout, and LayoutChatBars, on resize.
		auto event = QResizeEvent(chat->size(), chat->size());
		QCoreApplication::sendEvent(chat.data(), &event);
	}
}

object_ptr<Ui::RpWidget> PluginUi::build(
		Object &object,
		QWidget *parent,
		const QJsonObject &node,
		int depth) {
	if (depth > kMaximumDepth || ++object.count > kMaximumNodes) {
		throw std::runtime_error("UI tree is too deep or too large.");
	}
	const auto type = node.value(u"type"_q).toString();
	const auto id = node.value(u"id"_q).toString();
	const auto text = node.value(u"text"_q).toString();
	const auto objectId = object.objectId;
	// A dialog belongs to the window, not to us, and can outlive us.
	const auto guard = QPointer<PluginUi>(this);
	const auto sender = [=](const QString &kind, QJsonValue value) {
		if (!guard || _updating || InstallationUiActive() || Core::App().passcodeLocked()) {
			// Changes made by ui.update aren't reported back, so a plugin
			// reacting to them can't loop on its own updates.
			return;
		} else if (const auto i = _objects.find(objectId);
				i != end(_objects) && !i->second->silent) {
			send(*i->second, id, kind, value);
		}
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
			auto widget = build(object, column.data(), child, depth + 1);
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
			row->add(build(object, row.data(), child, depth + 1), size,
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
			object.texts[id] = value;
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
	if (id.isEmpty() || object.nodes.contains(id)) {
		throw std::runtime_error("Invalid or repeated node ID.");
	}
	if (node.contains(u"enabled"_q)) result->setEnabled(node.value(u"enabled"_q).toBool());
	if (node.contains(u"tooltip"_q)) result->setToolTip(node.value(u"tooltip"_q).toString());
	object.nodes.emplace(id, result.data());
	return result;
}

void PluginUi::update(const QJsonObject &command) {
	const auto i = _objects.find(command.value(u"object_id"_q).toString());
	if (i == end(_objects)) {
		return; // Closed meanwhile, Python learns it from the "closed" event.
	}
	const auto &object = *i->second;
	const auto nodeId = command.value(u"node_id"_q).toString();
	const auto node = object.nodes.find(nodeId);
	if (node == end(object.nodes) || !node->second) {
		throw std::runtime_error("Unknown UI node.");
	}
	const auto widget = node->second.data();
	const auto props = command.value(u"props"_q).toObject();
	const auto was = std::exchange(_updating, true);
	const auto restore = gsl::finally([&] { _updating = was; });
	if (props.contains(u"text"_q)) {
		const auto text = props.value(u"text"_q).toString();
		if (const auto label = dynamic_cast<Ui::FlatLabel*>(widget)) {
			label->setText(text);
		} else if (const auto field = dynamic_cast<Ui::InputField*>(widget)) {
			field->setText(text);
		} else if (const auto value = object.texts.find(nodeId);
				value != end(object.texts)) {
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
	const auto parent = object.overlay ? nullptr : widget->parentWidget();
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
	if (object.bar) {
		relayoutChats();
	}
}

void PluginUi::close(const QString &objectId, bool notify) {
	const auto i = _objects.find(objectId);
	if (i == end(_objects)) {
		return;
	}
	const auto object = i->second.get();
	object->silent = object->silent || !notify;
	if (object->box) {
		object->box->closeBox(); // destroyed() erases it later.
		return;
	}
	if (!object->silent) {
		send(*object, QString(), u"closed"_q);
	}
	// Deleted after erase(): their destroyed() handler looks up _objects.
	auto inserted = object->inserted;
	auto bar = std::move(object->bar);
	auto overlay = std::move(object->overlay);
	auto section = std::move(object->section);
	_objects.erase(i);
	delete inserted.data();
	overlay = nullptr;
	if (section) {
		// Leaves the window history and the screen.
		_sectionClosed.fire_copy(objectId);
		section = nullptr;
	}
	if (bar) {
		bar = nullptr;
		relayoutChats();
	}
}

void PluginUi::closePlugin(const QString &installationId) {
	auto ids = std::vector<QString>();
	for (const auto &[id, object] : _objects) {
		if (object->installationId == installationId) {
			ids.push_back(id);
		}
	}
	for (const auto &id : ids) {
		close(id, false);
	}
}

void PluginUi::destroyed(const QString &objectId) {
	const auto i = _objects.find(objectId);
	if (i == end(_objects)) {
		return;
	}
	if (!i->second->silent) {
		send(*i->second, QString(), u"closed"_q);
	}
	i->second->silent = true;
	// QWidget emits destroyed() from its own destructor, before QPointer is
	// cleared, so the bar must be released here, not deleted a second time.
	[[maybe_unused]] const auto released = i->second->bar.release();
	[[maybe_unused]] const auto overlay = i->second->overlay.release();
	const auto section = i->second->section.release();
	_objects.erase(i);
	if (section) {
		_sectionClosed.fire_copy(objectId);
	}
}

Ui::RpWidget *PluginUi::sectionContent(const QString &objectId) const {
	const auto i = _objects.find(objectId);
	return (i != end(_objects)) ? i->second->section.get() : nullptr;
}

QString PluginUi::sectionTitle(const QString &objectId) const {
	const auto i = _objects.find(objectId);
	return (i != end(_objects)) ? i->second->title : QString();
}

rpl::producer<> PluginUi::sectionClosed(const QString &objectId) const {
	return _sectionClosed.events(
	) | rpl::filter([=](const QString &id) {
		return id == objectId;
	}) | rpl::to_empty;
}

void PluginUi::sectionGone(const QString &objectId) {
	// Deferred: called while the window changes its history.
	crl::on_main(this, [=] {
		close(objectId, true);
	});
}

void PluginUi::send(
		const Object &object,
		const QString &nodeId,
		const QString &type,
		QJsonValue value) {
	auto event = QJsonObject{
		{ u"installation_id"_q, object.installationId },
		{ u"object_id"_q, object.objectId },
		{ u"node_id"_q, nodeId },
		{ u"type"_q, type },
	};
	if (!value.isNull() && !value.isUndefined()) {
		event.insert(u"value"_q, value);
	}
	_send(std::move(event));
}

} // namespace Ovygram
