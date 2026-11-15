#include "ovygram/plugin_commands.h"

#include "base/event_filter.h"
#include "core/application.h"
#include "core/plugin_runtime.h"
#include "data/data_peer.h"
#include "main/main_session.h"
#include "ovygram/plugin_elements.h"
#include "ovygram/plugin_install.h"
#include "ovygram/plugin_menus.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "styles/style_basic.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QTextEdit>

#include <stdexcept>
#include <vector>

namespace Ovygram::Commands {
namespace {

constexpr auto kVisibleRows = 6;

[[nodiscard]] QJsonObject Request(QJsonObject request) {
	const auto runtime = Core::App().pluginRuntime();
	if (!runtime) {
		throw std::runtime_error("Plugins are not available.");
	}
	const auto response = QJsonDocument::fromJson(QByteArray::fromStdString(
		runtime->dispatch(QJsonDocument(request).toJson(
			QJsonDocument::Compact).toStdString()))).object();
	if (!response.contains(u"result"_q)) {
		throw std::runtime_error(
			response.value(u"error"_q).toString().toStdString());
	}
	return response.value(u"result"_q).toObject();
}

struct Row {
	QString title;
	QString hint;
	QString description;
	QString plugin;
};

class Panel final : public Ui::RpWidget {
public:
	Panel(QWidget *parent, Fn<void(int)> chosen)
	: RpWidget(parent)
	, _chosen(std::move(chosen)) {
		setFocusPolicy(Qt::NoFocus);
		setMouseTracking(true);
	}

	void setView(const QJsonObject &view) {
		_rows.clear();
		for (const auto &value : view.value(u"items"_q).toArray()) {
			const auto item = value.toObject();
			_rows.push_back({
				item.value(u"title"_q).toString(),
				item.value(u"hint"_q).toString(),
				item.value(u"description"_q).toString(),
				item.value(u"plugin"_q).toString(),
			});
		}
		_selected = std::clamp(
			view.value(u"selected"_q).toInt(),
			0,
			std::max(int(_rows.size()) - 1, 0));
		_footer = view.value(u"footer"_q).toString();
		_error = view.value(u"error"_q).toBool();
		// Keep the selected row inside the visible ones.
		if (_selected < _offset) {
			_offset = _selected;
		} else if (_selected >= _offset + kVisibleRows) {
			_offset = _selected - kVisibleRows + 1;
		}
		_offset = std::clamp(
			_offset,
			0,
			std::max(int(_rows.size()) - kVisibleRows, 0));
		update();
	}

	[[nodiscard]] int desiredHeight() const {
		const auto rows = std::min(int(_rows.size()), kVisibleRows);
		return (rows + (_footer.isEmpty() ? 0 : 1)) * rowHeight()
			+ st::lineWidth;
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = Painter(this);
		p.fillRect(rect(), st::windowBg);
		p.fillRect(0, 0, width(), st::lineWidth, st::shadowFg);
		const auto height = rowHeight();
		const auto padding = this->padding();
		const auto available = width() - 2 * padding;
		const auto rows = std::min(int(_rows.size()), kVisibleRows);
		auto top = int(st::lineWidth);
		for (auto i = 0; i != rows; ++i, top += height) {
			const auto &row = _rows[_offset + i];
			if (_offset + i == _selected) {
				p.fillRect(0, top, width(), height, st::windowBgOver);
			}
			const auto baseline = top
				+ (height - st::normalFont->height) / 2
				+ st::normalFont->ascent;
			auto left = padding;
			auto right = padding + available;
			if (!row.plugin.isEmpty()) {
				const auto text = st::normalFont->elided(row.plugin, available / 3);
				right -= st::normalFont->width(text);
				p.setFont(st::normalFont);
				p.setPen(st::windowSubTextFg);
				p.drawText(right, baseline, text);
				right -= padding;
			}
			const auto draw = [&](
					const QString &text,
					const style::font &font,
					const style::color &color) {
				if (text.isEmpty() || right <= left) {
					return;
				}
				const auto elided = font->elided(text, right - left);
				p.setFont(font);
				p.setPen(color);
				p.drawText(left, baseline, elided);
				left += font->width(elided) + font->spacew;
			};
			draw(row.title, st::semiboldFont, st::windowFg);
			draw(row.hint, st::normalFont, st::windowSubTextFg);
			if (!row.description.isEmpty()) {
				draw(u"— "_q + row.description, st::normalFont, st::windowSubTextFg);
			}
		}
		if (!_footer.isEmpty()) {
			p.setFont(st::normalFont);
			p.setPen(_error ? st::attentionButtonFg : st::windowSubTextFg);
			p.drawText(
				padding,
				top + (height - st::normalFont->height) / 2 + st::normalFont->ascent,
				st::normalFont->elided(_footer, available));
		}
	}

	void mousePressEvent(QMouseEvent *e) override {
		const auto row = (e->pos().y() - st::lineWidth) / rowHeight();
		if (e->button() == Qt::LeftButton
			&& row >= 0
			&& row < std::min(int(_rows.size()), kVisibleRows)) {
			_chosen(_offset + row);
		}
	}

private:
	[[nodiscard]] int rowHeight() const {
		return st::normalFont->height * 2;
	}
	[[nodiscard]] int padding() const {
		return st::normalFont->height * 3 / 4;
	}

	const Fn<void(int)> _chosen;
	std::vector<Row> _rows;
	QString _footer;
	int _selected = 0;
	int _offset = 0;
	bool _error = false;

};

// The grey text after the cursor. It is not part of the field's text.
class Ghost final : public QWidget {
public:
	explicit Ghost(not_null<QTextEdit*> edit)
	: QWidget(edit->viewport())
	, _edit(edit) {
		setAttribute(Qt::WA_TransparentForMouseEvents);
		hide();
	}

	void setText(const QString &text) {
		_text = text;
		if (_text.isEmpty()) {
			hide();
			return;
		}
		const auto cursor = _edit->cursorRect();
		const auto left = cursor.x() + cursor.width();
		setGeometry(
			left,
			cursor.y(),
			std::max(_edit->viewport()->width() - left, 0),
			cursor.height());
		show();
		raise();
		update();
	}

protected:
	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		const auto font = _edit->textCursor().charFormat().font();
		p.setFont(font);
		p.setPen(st::windowSubTextFg->c);
		p.drawText(
			rect(),
			Qt::AlignLeft | Qt::AlignVCenter,
			QFontMetrics(font).elidedText(_text, Qt::ElideRight, width()));
	}

private:
	const not_null<QTextEdit*> _edit;
	QString _text;

};

class Controller;

std::vector<QPointer<Controller>> &Controllers() {
	// Leaked on purpose: fields are destroyed during the shutdown too.
	static const auto result = new std::vector<QPointer<Controller>>();
	return *result;
}

class Controller final : public QObject {
public:
	Controller(
		not_null<QWidget*> wrap,
		not_null<Ui::InputField*> field,
		Fn<PeerData*()> peer);

	[[nodiscard]] Ui::InputField *field() const {
		return _field;
	}
	[[nodiscard]] bool submit();
	void refresh();

private:
	[[nodiscard]] QJsonObject context(bool full) const;
	[[nodiscard]] bool key(const QString &name);
	void changed();
	void apply(const QJsonObject &view);
	void applyResult(const QJsonObject &result);
	void hide();
	void place();

	const QPointer<QWidget> _wrap;
	const not_null<Ui::InputField*> _field;
	const Fn<PeerData*()> _peer;
	QPointer<Panel> _panel;
	QPointer<Ghost> _ghost;
	bool _active = false;
	bool _hasItems = false;
	bool _applying = false;
	bool _consumed = false;

};

Controller::Controller(
	not_null<QWidget*> wrap,
	not_null<Ui::InputField*> field,
	Fn<PeerData*()> peer)
: QObject(field)
, _wrap(wrap)
, _field(field)
, _peer(std::move(peer)) {
	_field->changes() | rpl::on_next([=] {
		changed();
	}, _field->lifetime());

	// Installed after the field's own handlers, so it sees the keys first.
	base::install_event_filter(this, _field->rawTextEdit(), [=](
			not_null<QEvent*> e) {
		using Result = base::EventFilterResult;
		const auto type = e->type();
		if (type == QEvent::FocusOut) {
			hide();
			return Result::Continue;
		} else if (!_active
			|| (type != QEvent::KeyPress && type != QEvent::ShortcutOverride)) {
			return Result::Continue;
		}
		const auto event = static_cast<QKeyEvent*>(e.get());
		if (event->modifiers() & ~Qt::KeypadModifier) {
			return Result::Continue;
		}
		const auto name = (event->key() == Qt::Key_Escape)
			? u"escape"_q
			: !_hasItems
			? QString()
			: (event->key() == Qt::Key_Up)
			? u"up"_q
			: (event->key() == Qt::Key_Down)
			? u"down"_q
			: (event->key() == Qt::Key_Tab)
			? u"tab"_q
			: QString();
		if (name.isEmpty()) {
			return Result::Continue;
		} else if (type == QEvent::ShortcutOverride) {
			// Deliver the key here instead of to a Telegram shortcut.
			event->accept();
			return Result::Cancel;
		}
		return key(name) ? Result::Cancel : Result::Continue;
	});
	base::install_event_filter(this, wrap, [=](not_null<QEvent*> e) {
		if (e->type() == QEvent::Resize || e->type() == QEvent::Move) {
			place();
		} else if (e->type() == QEvent::Hide) {
			hide();
		}
		return base::EventFilterResult::Continue;
	});
}

QJsonObject Controller::context(bool full) const {
	const auto peer = _peer();
	if (!peer) {
		return {};
	}
	auto result = QJsonObject{
		{ u"kind"_q, peerIsChannel(peer->id)
			? u"channel"_q
			: peerIsChat(peer->id)
			? u"chat"_q
			: u"user"_q },
		{ u"account_id"_q, QString::number(peer->session().uniqueId()) },
		{ u"chat_id"_q, QString::number(peer->id.value) },
	};
	if (full) {
		result.insert(u"chat"_q, Menus::DescribeContext(
			peer->session(),
			peer,
			nullptr).value(u"chat"_q));
	}
	return result;
}

void Controller::changed() {
	if (_applying) {
		return;
	}
	const auto &text = _field->getLastText();
	if ((!_active && !text.startsWith('/'))
		|| !_field->rawTextEdit()->hasFocus()
		|| InstallationUiActive()) {
		hide();
		return;
	}
	const auto current = context(false);
	if (current.isEmpty()) {
		hide();
		return;
	}
	try {
		apply(Request({
			{ u"action"_q, u"commands_input"_q },
			{ u"text"_q, text },
			{ u"cursor"_q, Elements::UnitsToCodePoints(
				text,
				_field->textCursor().position()) },
			{ u"context"_q, current },
		}));
	} catch (const std::exception &) {
		hide();
	}
}

bool Controller::key(const QString &name) {
	try {
		applyResult(Request({
			{ u"action"_q, u"commands_key"_q },
			{ u"key"_q, name },
		}));
		return true;
	} catch (const std::exception &) {
		hide();
		return false;
	}
}

void Controller::applyResult(const QJsonObject &result) {
	if (result.contains(u"text"_q)) {
		const auto text = result.value(u"text"_q).toString();
		_applying = true;
		_field->setTextWithTags({ text });
		auto cursor = _field->textCursor();
		cursor.setPosition(Elements::CodePointsToUnits(
			text,
			result.value(u"cursor"_q).toInt()));
		_field->setTextCursor(cursor);
		_applying = false;
	}
	apply(result.value(u"view"_q).toObject());
}

void Controller::apply(const QJsonObject &view) {
	if (!view.value(u"active"_q).toBool() || !_wrap) {
		hide();
		return;
	}
	_active = true;
	_hasItems = !view.value(u"items"_q).toArray().isEmpty();
	if (!_panel) {
		_panel = Ui::CreateChild<Panel>(_wrap->parentWidget(), [=](int index) {
			try {
				applyResult(Request({
					{ u"action"_q, u"commands_pick"_q },
					{ u"index"_q, index },
				}));
			} catch (const std::exception &) {
				hide();
			}
			_field->setFocus();
		});
	}
	_panel->setView(view);
	place();
	if (!_ghost) {
		_ghost = new Ghost(_field->rawTextEdit());
	}
	_ghost->setText(view.value(u"ghost"_q).toString());
}

void Controller::place() {
	if (!_panel || !_wrap || !_active) {
		return;
	}
	const auto height = _panel->desiredHeight();
	if (height <= st::lineWidth) {
		_panel->hide();
		return;
	}
	_panel->setGeometry(
		_wrap->x(),
		_wrap->y() - height,
		_wrap->width(),
		height);
	_panel->show();
	_panel->raise();
}

void Controller::hide() {
	_active = false;
	_hasItems = false;
	if (_panel) {
		_panel->hide();
	}
	if (_ghost) {
		_ghost->setText(QString());
	}
}

void Controller::refresh() {
	if (!_active) {
		return;
	}
	try {
		apply(Request({ { u"action"_q, u"commands_view"_q } }));
	} catch (const std::exception &) {
		hide();
	}
}

bool Controller::submit() {
	if (_consumed) {
		// The same click or Enter, seen by another subscriber.
		return true;
	}
	const auto text = _field->getLastText();
	const auto current = text.startsWith('/') ? context(true) : QJsonObject();
	if (current.isEmpty() || InstallationUiActive()) {
		return false;
	}
	auto result = QJsonObject();
	try {
		result = Request({
			{ u"action"_q, u"commands_submit"_q },
			{ u"text"_q, text },
			{ u"context"_q, current },
		});
	} catch (const std::exception &) {
		return false;
	}
	if (!result.value(u"handled"_q).toBool()) {
		return false;
	}
	_consumed = true;
	QMetaObject::invokeMethod(this, [=] {
		_consumed = false;
	}, Qt::QueuedConnection);
	const auto error = result.value(u"error"_q).toString();
	if (result.contains(u"text"_q)) {
		// Enter on an unfinished name completed it.
		applyResult(result);
	} else if (error.isEmpty()) {
		_applying = true;
		_field->clear();
		_applying = false;
		hide();
	} else {
		// The text stays in the field with the reason above it.
		apply(QJsonObject{
			{ u"active"_q, true },
			{ u"items"_q, QJsonArray() },
			{ u"footer"_q, error },
			{ u"error"_q, true },
		});
	}
	return true;
}

[[nodiscard]] Controller *Find(Ui::InputField *field) {
	auto &list = Controllers();
	std::erase_if(list, [](const QPointer<Controller> &entry) {
		return !entry;
	});
	for (const auto &entry : list) {
		if (!field || entry->field() == field) {
			if (field || entry->field()->rawTextEdit()->hasFocus()) {
				return entry.data();
			}
		}
	}
	return nullptr;
}

} // namespace

void Attach(
		not_null<QWidget*> wrap,
		not_null<Ui::InputField*> field,
		Fn<PeerData*()> peer) {
	Controllers().emplace_back(new Controller(wrap, field, std::move(peer)));
}

bool Submit(not_null<Ui::InputField*> field) {
	const auto controller = Find(field);
	return controller && controller->submit();
}

void Refresh() {
	if (const auto controller = Find(nullptr)) {
		controller->refresh();
	}
}

} // namespace Ovygram::Commands
