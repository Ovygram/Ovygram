#include "ovygram/plugin_elements.h"
#include "ovygram/plugin_install.h"
#include "ovygram/plugin_widgets.h"
#include "data/data_peer.h"
#include "core/application.h"
#include "main/main_session.h"
#include <QtCore/QJsonArray>
#include "ovygram/protected_ui.h"

#include "ui/style/style_core.h"
#include "ui/widgets/fields/input_field.h"

#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
#include <QtWidgets/QTextEdit>
#include "ui/style/style_core_palette.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QPointer>
#include <QtGui/QColor>
#include <QtGui/QMouseEvent>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Ovygram {
namespace {

struct Element {
	QString name;
	QPointer<QWidget> widget;
	bool hiddenByRule = false;
	bool wanted = true; // Visibility Telegram asked for while we hid it.
};

struct Rules {
	std::set<QString> hiding; // Plugin IDs.
	std::map<QString, bool> clicks; // Plugin ID -> replace the action.
};

class Filter final : public QObject {
protected:
	bool eventFilter(QObject *watched, QEvent *e) override;
};

struct Editor {
	QString kind;
	std::function<PeerData*()> peer;
	std::function<QJsonObject()> snapshot;
	std::function<bool(const QJsonObject&)> action;
};

struct State {
	std::map<Ui::InputField*, Editor> editors;
	std::map<QString, QString> composeWatchers; // Rule ID -> plugin ID.
	bool editing = false; // Applying a plugin's compose command.
	std::vector<Element> elements;
	std::map<QString, Rules> rules;
	std::function<void(QJsonObject)> send;
	Filter filter;
	std::optional<QByteArray> basePalette;
	std::map<QString, std::vector<std::pair<QByteArray, QColor>>> paletteOverrides;
	bool paletteSuspended = false;
	bool applyingPalette = false;
	rpl::lifetime protection;
};

State &Get() {
	// Leaked on purpose: the filter must outlive every watched widget.
	static const auto result = new State();
	return *result;
}

[[nodiscard]] bool Hidden(const QString &name) {
	const auto &rules = Get().rules;
	const auto i = rules.find(name);
	return (i != end(rules)) && !i->second.hiding.empty();
}

void Cleanup() {
	std::erase_if(Get().elements, [](const Element &element) {
		return !element.widget;
	});
}

// Returns true when the visibility changed.
bool Apply(Element &element) {
	const auto widget = element.widget.data();
	if (!widget) {
		return false;
	} else if (Hidden(element.name)) {
		if (!widget->isHidden()) {
			element.wanted = true;
			widget->hide();
			element.hiddenByRule = true;
			return true;
		} else if (!element.hiddenByRule) {
			element.wanted = false;
			element.hiddenByRule = true;
		}
	} else if (element.hiddenByRule) {
		element.hiddenByRule = false;
		if (element.wanted) {
			widget->show();
			return true;
		}
	}
	return false;
}

void Relayout(QWidget *owner) {
	if (owner) {
		// Telegram widgets recompute their layout on resize.
		auto event = QResizeEvent(owner->size(), owner->size());
		QCoreApplication::sendEvent(owner, &event);
	}
}

void ApplyAll(const QString &name) {
	Cleanup();
	auto owners = std::vector<QPointer<QWidget>>();
	for (auto &element : Get().elements) {
		if (element.name == name && Apply(element)) {
			owners.emplace_back(element.widget->parentWidget());
		}
	}
	for (const auto &owner : owners) {
		Relayout(owner.data());
	}
}

[[nodiscard]] Element *Find(QObject *widget) {
	for (auto &element : Get().elements) {
		if (element.widget.data() == widget) {
			return &element;
		}
	}
	return nullptr;
}

bool Filter::eventFilter(QObject *watched, QEvent *e) {
	if (InstallationUiActive() || Core::App().passcodeLocked() || InsideProtected(watched)) return false;
	const auto type = e->type();
	if (type == QEvent::Show) {
		const auto element = Find(watched);
		if (element && Hidden(element->name)) {
			// Shown outside the owner's visibility pass: hide it again.
			const auto widget = element->widget;
			const auto name = element->name;
			QMetaObject::invokeMethod(this, [=] {
				if (widget && Hidden(name) && !widget->isHidden()) {
					ApplyAll(name);
				}
			}, Qt::QueuedConnection);
		}
	} else if (type == QEvent::MouseButtonPress
		|| type == QEvent::MouseButtonRelease) {
		const auto mouse = static_cast<QMouseEvent*>(e);
		const auto element = Find(watched);
		if (!element || mouse->button() != Qt::LeftButton) {
			return false;
		}
		const auto i = Get().rules.find(element->name);
		if (i == end(Get().rules) || i->second.clicks.empty()) {
			return false;
		}
		auto replace = false;
		for (const auto &[installationId, replaces] : i->second.clicks) {
			replace = replace || replaces;
		}
		const auto inside = element->widget->rect().contains(
			mouse->position().toPoint());
		if (type == QEvent::MouseButtonRelease && inside && Get().send) {
			const auto name = element->name;
			auto plugins = std::vector<QString>();
			for (const auto &[installationId, replaces] : i->second.clicks) {
				plugins.push_back(installationId);
			}
			for (const auto &installationId : plugins) {
				Get().send(QJsonObject{
					{ u"installation_id"_q, installationId },
					{ u"type"_q, u"element_click"_q },
					{ u"element"_q, name },
				});
			}
		}
		return replace;
	}
	return false;
}

// The element shown in the active window, or any shown one.
[[nodiscard]] QWidget *FindShown(const QString &name) {
	Cleanup();
	auto fallback = (QWidget*)nullptr;
	for (const auto &element : Get().elements) {
		const auto widget = element.widget.data();
		if (element.name != name || !widget || !widget->isVisible()) {
			continue;
		} else if (widget->isActiveWindow()) {
			return widget;
		} else if (!fallback) {
			fallback = widget;
		}
	}
	return fallback;
}

// QTextDocument positions count UTF-16 code units, Python indexes str by
// code points: an emoji outside the BMP is 2 units and 1 code point.
[[nodiscard]] int UnitsToCodePoints(const QString &text, int units) {
	units = std::clamp(units, 0, int(text.size()));
	auto result = 0;
	for (auto i = 0; i < units; ++i, ++result) {
		if (text[i].isHighSurrogate()
			&& i + 1 < text.size()
			&& text[i + 1].isLowSurrogate()) {
			++i;
		}
	}
	return result;
}

[[nodiscard]] int CodePointsToUnits(const QString &text, int points) {
	auto i = 0;
	for (; i < text.size() && points > 0; ++i, --points) {
		if (text[i].isHighSurrogate()
			&& i + 1 < text.size()
			&& text[i + 1].isLowSurrogate()) {
			++i;
		}
	}
	return i;
}

// Text is the document text (custom emoji are U+FFFC there), positions are
// code point indexes into it, as Python str indexes.
[[nodiscard]] QJsonObject FieldState(not_null<Ui::InputField*> field) {
	const auto cursor = field->textCursor();
	const auto text = field->rawTextEdit()->toPlainText();
	return {
		{ u"text"_q, text },
		{ u"cursor"_q, UnitsToCodePoints(text, cursor.position()) },
		{ u"selection_start"_q, UnitsToCodePoints(text, cursor.selectionStart()) },
		{ u"selection_end"_q, UnitsToCodePoints(text, cursor.selectionEnd()) },
	};
}

void WatchField(not_null<Ui::InputField*> field) {
	field->changes() | rpl::on_next([=] {
		auto &state = Get();
		// Only edits by the user: the focus is on the inner QTextEdit.
		if (state.composeWatchers.empty()
			|| !state.send
			|| state.editing
			|| InstallationUiActive()
			|| Core::App().passcodeLocked()
			|| ProtectedSurfaceExists()
			|| !field->rawTextEdit()->hasFocus()) {
			return;
		}
		const auto value = FieldState(field);
		const auto watchers = state.composeWatchers;
		for (const auto &[ruleId, installationId] : watchers) {
			state.send(QJsonObject{
				{ u"installation_id"_q, installationId },
				{ u"type"_q, u"compose_changed"_q },
				{ u"rule_id"_q, ruleId },
				{ u"value"_q, value },
			});
		}
	}, field->lifetime());
}

[[nodiscard]] Ui::InputField *ComposeField() {
	const auto result = dynamic_cast<Ui::InputField*>(
		FindShown(u"compose.field"_q));
	if (!result) {
		throw std::runtime_error("No message field is shown.");
	}
	return result;
}

QJsonObject EditorState(Ui::InputField *field, const Editor &editor, PeerData *peer) {
	auto result = editor.snapshot();
	const auto &value = field->getTextWithTags();
	auto tags = QJsonArray();
	for (const auto &tag : value.tags) {
		tags.append(QJsonObject{ { u"offset"_q, tag.offset }, { u"length"_q, tag.length },
			{ u"id"_q, tag.id } });
	}
	result.insert(u"text"_q, value.text);
	result.insert(u"tags"_q, tags);
	result.insert(u"field"_q, Widgets::HandleFor(field));
	result.insert(u"account_id"_q, QString::number(peer->session().uniqueId()));
	result.insert(u"chat_id"_q, QString::number(peer->id.value));
	result.insert(u"kind"_q, editor.kind);
	result.insert(u"revision"_q, field->rawTextEdit()->document()->revision());
	const auto cursor = field->textCursor();
	result.insert(u"selection"_q, QJsonArray{ cursor.selectionStart(), cursor.selectionEnd() });
	return result;
}

TextWithTags ParseDraft(const QJsonObject &command) {
	if (!command.value(u"text"_q).isString() || !command.value(u"tags"_q).isArray()) {
		throw std::runtime_error("Expected draft text and tags.");
	}
	auto result = TextWithTags{ command.value(u"text"_q).toString(), {} };
	const auto tags = command.value(u"tags"_q).toArray();
	if (result.text.size() > 65536 || tags.size() > 4096) throw std::runtime_error("Draft too large.");
	auto end = 0;
	for (const auto value : tags) {
		const auto tag = value.toObject();
		const auto start = tag.value(u"offset"_q).toInt(-1);
		const auto length = tag.value(u"length"_q).toInt(-1);
		const auto name = tag.value(u"id"_q).toString();
		if (start < end || length <= 0 || start > result.text.size() - length
			|| name.isEmpty() || name.size() > 4096
			|| result.text[start].isLowSurrogate()
			|| (start + length < result.text.size() && result.text[start + length].isLowSurrogate())) {
			throw std::runtime_error("Invalid draft tag range.");
		}
		result.tags.push_back({start, length, name});
		end = start + length;
	}
	return result;
}

QJsonValue EditorRequest(const QJsonObject &command) {
	if (Core::App().passcodeLocked() || ProtectedSurfaceExists() || InstallationUiActive()) {
		throw std::runtime_error("Editor access is suspended while a protected surface is open.");
	}
	const auto expected = command.value(u"expected"_q).toObject();
	const auto fieldId = expected.value(u"field"_q);
	const auto requested = fieldId.isUndefined() ? nullptr : Widgets::Resolve(fieldId);
	const auto write = command.value(u"method"_q).toString() == u"compose.apply"_q;
	if (write && (!requested || expected.isEmpty())) throw std::runtime_error("Editor has closed.");
	for (const auto &[field, editor] : Get().editors) {
		if ((requested && requested != field) || !field->isActiveWindow() || !field->isVisible()
			|| editor.kind != command.value(u"kind"_q).toString()) continue;
		const auto peer = editor.peer();
		if (!peer || QString::number(peer->session().uniqueId()) != command.value(u"account_id"_q).toString()
			|| QString::number(peer->id.value) != command.value(u"chat_id"_q).toString()) continue;
		const auto observed = EditorState(field, editor, peer);
		if (!write) return observed;
		if (expected != observed) throw std::runtime_error("The draft or selection has changed.");
		const auto op = command.value(u"op"_q).toString();
		if (op == u"text"_q) {
			if (field->rawTextEdit()->isReadOnly()) throw std::runtime_error("Read-only field.");
			const auto text = ParseDraft(command);
			const auto was = std::exchange(Get().editing, true);
			const auto restore = gsl::finally([&] { Get().editing = was; });
			field->setTextWithTags(text);
		} else if (op == u"suggestion"_q) {
			const auto edit = field->rawTextEdit();
			const auto text = command.value(u"text"_q).toString();
			if (edit->isReadOnly() || !edit->hasFocus() || text.size() > 65536
				|| !command.value(u"text"_q).isString()) {
				throw std::runtime_error("Suggestion unavailable.");
			}
			const auto was = std::exchange(Get().editing, true);
			const auto restore = gsl::finally([&] { Get().editing = was; });
			auto cursor = edit->textCursor();
			const auto start = cursor.selectionStart();
			cursor.insertText(text);
			const auto end = cursor.position();
			cursor.setPosition(start);
			cursor.setPosition(end, QTextCursor::KeepAnchor);
			edit->setTextCursor(cursor);
		} else if (!editor.action(command)) {
			throw std::runtime_error("Editor action unavailable.");
		}
		return true;
	}
	if (write) throw std::runtime_error("Editor is no longer active.");
	return QJsonValue(QJsonValue::Null);
}

void ApplyStoredColors(State &state) {
	if (!state.basePalette) {
		return;
	}
	const auto guard = gsl::finally([&] { state.applyingPalette = false; });
	state.applyingPalette = true;
	style::main_palette::load(*state.basePalette);
	if (!state.paletteSuspended) {
		for (const auto &[owner, entries] : state.paletteOverrides) {
			for (const auto &[name, color] : entries) {
				style::main_palette::setColor(
					QLatin1String(name.constData(), name.size()),
					uchar(color.red()),
					uchar(color.green()),
					uchar(color.blue()),
					uchar(color.alpha()));
			}
		}
	}
	style::NotifyPaletteChanged();
}

// Protected surfaces are drawn in the colors of the user's theme: a plugin
// palette could hide a warning or recolor a trust badge.
void SuspendColors(bool suspended) {
	auto &state = Get();
	if (suspended == state.paletteSuspended) {
		return;
	}
	state.paletteSuspended = suspended;
	if (!state.paletteOverrides.empty()) {
		ApplyStoredColors(state);
	}
}

void EnsurePaletteListeners(State &state) {
	if (state.protection) {
		return;
	}
	ProtectedSurfacesChanged() | rpl::on_next([] {
		SuspendColors(ProtectedSurfaceExists() || Core::App().passcodeLocked());
	}, state.protection);
	style::PaletteChanged() | rpl::on_next([] {
		auto &state = Get();
		if (state.applyingPalette || !state.basePalette) {
			return;
		}
		// User switched themes while plugin overrides exist: record the new
		// base theme instead of later rolling back to an old theme snapshot.
		state.basePalette = style::main_palette::save();
		if (!state.paletteSuspended && !state.paletteOverrides.empty()) {
			ApplyStoredColors(state);
		}
	}, state.protection);
}

void SetColors(const QString &installationId, const QJsonObject &colors) {
	auto &state = Get();
	if (ProtectedSurfaceExists() || Core::App().passcodeLocked()) {
		// Its texts and warnings keep the colors of the user's theme.
		throw std::runtime_error(
			"Colors can't be changed while a protected screen is open.");
	}
	EnsurePaletteListeners(state);
	auto parsed = std::vector<std::pair<QByteArray, QColor>>();
	for (auto i = colors.begin(); i != colors.end(); ++i) {
		const auto color = QColor(i.value().toString());
		if (!color.isValid()) {
			throw std::runtime_error("Invalid color, use #rrggbb or #aarrggbb.");
		}
		parsed.emplace_back(i.key().toLatin1(), color);
	}
	// Validate every key before mutating state.
	const auto before = style::main_palette::save();
	for (const auto &[name, color] : parsed) {
		const auto result = style::main_palette::setColor(
			QLatin1String(name.constData(), name.size()),
			uchar(color.red()),
			uchar(color.green()),
			uchar(color.blue()),
			uchar(color.alpha()));
		if (result == style::palette::SetResult::KeyNotFound) {
			style::main_palette::load(before);
			throw std::runtime_error(
				"Unknown palette color: " + name.toStdString());
		}
	}
	if (!state.basePalette) {
		state.basePalette = before;
	}
	state.paletteOverrides[installationId] = std::move(parsed);
	ApplyStoredColors(state);
}

void ResetColors(const QString &installationId) {
	auto &state = Get();
	if (!state.paletteOverrides.erase(installationId)) {
		return;
	}
	if (!state.basePalette) {
		return;
	}
	if (state.paletteOverrides.empty()) {
		const auto wasSuspended = state.paletteSuspended;
		state.paletteSuspended = false;
		if (!wasSuspended) {
			ApplyStoredColors(state);
		}
		state.basePalette.reset();
	} else if (!state.paletteSuspended) {
		ApplyStoredColors(state);
	}
}

} // namespace

void RegisterElements(
		std::initializer_list<std::pair<const char*, QWidget*>> elements) {
	auto &state = Get();
	Cleanup();
	for (const auto &[name, widget] : elements) {
		if (widget) {
			state.elements.push_back({ QString::fromLatin1(name), widget });
			widget->installEventFilter(&state.filter);
			// Lets generic widget selectors match by element name.
			widget->setProperty("_ovygram_element", QString::fromLatin1(name));
			if (const auto field = dynamic_cast<Ui::InputField*>(widget)) {
				WatchField(field);
			}
		}
	}
}

void RegisterEditor(Ui::InputField *field, QString kind, std::function<PeerData*()> peer,
		std::function<QJsonObject()> snapshot, std::function<bool(const QJsonObject&)> action) {
	if (!field) {
		return;
	}
	Get().editors.emplace(field, Editor{ std::move(kind), std::move(peer), std::move(snapshot), std::move(action) });
	QObject::connect(field, &QObject::destroyed, [field] { Get().editors.erase(field); });
}

void ApplyElementRules(QWidget *owner) {
	Cleanup();
	for (auto &element : Get().elements) {
		if (element.widget && element.widget->parentWidget() == owner) {
			Apply(element);
		}
	}
}

namespace Elements {

void SetEventSender(std::function<void(QJsonObject)> send) {
	Get().send = std::move(send);
}

void Handle(const QJsonObject &command) {
	const auto method = command.value(u"method"_q).toString();
	const auto installationId = command.value(u"installation_id"_q).toString();
	const auto name = command.value(u"element"_q).toString();
	// Edits made by plugin commands aren't reported back as user edits, so a
	// plugin reacting to changes can't loop on its own edits.
	auto &editing = Get().editing;
	const auto wasEditing = std::exchange(editing, true);
	const auto restore = gsl::finally([&] { editing = wasEditing; });
	if (method == u"ui.element.hide"_q) {
		auto &hiding = Get().rules[name].hiding;
		if (command.value(u"hidden"_q).toBool()) {
			hiding.insert(installationId);
		} else {
			hiding.erase(installationId);
		}
		ApplyAll(name);
	} else if (method == u"ui.element.click"_q) {
		auto &clicks = Get().rules[name].clicks;
		if (command.value(u"enabled"_q).toBool()) {
			clicks[installationId] = command.value(u"replace"_q).toBool();
		} else {
			clicks.erase(installationId);
		}
	} else if (method == u"ui.compose.set"_q) {
		if (Core::App().passcodeLocked() || ProtectedSurfaceExists() || InstallationUiActive()) {
			throw std::runtime_error("Compose access is suspended while a protected surface is open.");
		}
		const auto field = ComposeField();
		const auto text = command.value(u"text"_q).toString();
		field->setTextWithTags({ text, {} });
		field->setCursorPosition(text.size());
		field->setFocus();
	} else if (method == u"ui.compose.insert"_q) {
		if (Core::App().passcodeLocked() || ProtectedSurfaceExists() || InstallationUiActive()) {
			throw std::runtime_error("Compose access is suspended while a protected surface is open.");
		}
		const auto field = ComposeField();
		field->insertTag(command.value(u"text"_q).toString());
		field->setFocus();
	} else if (method == u"ui.compose.replace"_q) {
		if (Core::App().passcodeLocked() || ProtectedSurfaceExists() || InstallationUiActive()) {
			throw std::runtime_error("Compose access is suspended while a protected surface is open.");
		}
		const auto field = ComposeField();
		const auto edit = field->rawTextEdit();
		auto cursor = edit->textCursor();
		// Python sends code point indexes, see FieldState().
		const auto current = edit->toPlainText();
		const auto from = CodePointsToUnits(
			current,
			command.value(u"start"_q).toInt());
		const auto till = std::max(
			CodePointsToUnits(current, command.value(u"end"_q).toInt()),
			from);
		const auto text = command.value(u"text"_q).toString();
		cursor.setPosition(from);
		cursor.setPosition(till, QTextCursor::KeepAnchor);
		cursor.insertText(text);
		if (command.value(u"select"_q).toBool()) {
			const auto insertedEnd = cursor.position();
			cursor.setPosition(from);
			cursor.setPosition(insertedEnd, QTextCursor::KeepAnchor);
		}
		edit->setTextCursor(cursor);
	} else if (method == u"ui.compose.watch"_q) {
		const auto ruleId = command.value(u"rule_id"_q).toString();
		if (const auto i = Get().composeWatchers.find(ruleId);
			i != end(Get().composeWatchers) && i->second != installationId) {
			throw std::runtime_error("Compose watcher belongs to another installation.");
		}
		if (command.value(u"enabled"_q).toBool()) {
			Get().composeWatchers[ruleId] = installationId;
		} else {
			Get().composeWatchers.erase(ruleId);
		}
	} else if (method == u"ui.palette"_q) {
		if (command.value(u"reset"_q).toBool()) {
			ResetColors(installationId);
		} else {
			SetColors(installationId, command.value(u"colors"_q).toObject());
		}
	} else {
		throw std::runtime_error("Unknown UI element command.");
	}
}

QJsonValue Request(const QJsonObject &command) {
	const auto method = command.value(u"method"_q).toString();
	if (method == u"compose.snapshot"_q || method == u"compose.apply"_q) return EditorRequest(command);
	if (Core::App().passcodeLocked() || ProtectedSurfaceExists() || InstallationUiActive()) {
		return QJsonValue(QJsonValue::Null);
	}
	const auto field = dynamic_cast<Ui::InputField*>(
		FindShown(u"compose.field"_q));
	if (method == u"compose.get_text"_q) {
		return field
			? QJsonValue(field->getTextWithTags().text)
			: QJsonValue(QJsonValue::Null);
	} else if (method == u"compose.get_state"_q) {
		return field ? QJsonValue(FieldState(field)) : QJsonValue(QJsonValue::Null);
	}
	throw std::runtime_error("Unknown element request.");
}

void RemovePlugin(const QString &installationId) {
	std::erase_if(Get().composeWatchers, [&](const auto &entry) {
		return entry.second == installationId;
	});
	auto changed = std::vector<QString>();
	for (auto &[name, rules] : Get().rules) {
		rules.clicks.erase(installationId);
		if (rules.hiding.erase(installationId)) {
			changed.push_back(name);
		}
	}
	for (const auto &name : changed) {
		ApplyAll(name);
	}
	ResetColors(installationId);
}

int UnitsToCodePoints(const QString &text, int units) {
	return Ovygram::UnitsToCodePoints(text, units);
}

int CodePointsToUnits(const QString &text, int points) {
	return Ovygram::CodePointsToUnits(text, points);
}

} // namespace Elements
} // namespace Ovygram
