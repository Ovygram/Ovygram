#include "ovygram/plugin_actions.h"

#include "base/global_shortcuts.h"
#include "base/weak_ptr.h"
#include "ovygram/i18n.h"
#include "ovygram/plugin_install.h"
#include "ovygram/plugin_widgets.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "main/main_session.h"
#include "ovygram/protected_ui.h"
#include "platform/platform_tray.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/labels.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/style_plugin_management.h"

#include "ui/toast/toast.h"
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <rpl/event_stream.h>
#include <algorithm>
#include <map>
#include <stdexcept>

namespace Ovygram::Actions {
namespace {

struct Action {
	quint64 generation = 0;
	QString owner;
	QString name;
	QString title;
	bool tray = false;
	bool link = false;
	bool global = false;
	bool local = false;
	base::GlobalShortcut binding;
};

struct State {
	std::map<QString, Action> entries;
	std::unique_ptr<base::GlobalShortcutManager> shortcuts;
	std::function<void(QJsonObject)> send;
	rpl::event_stream<> changes;
	bool refreshQueued = false;
	quint64 nextGeneration = 0;
};

State &Get() {
	static const auto state = new State();
	return *state;
}

void Refresh() {
	auto &state = Get();
	if (state.refreshQueued) return;
	state.refreshQueued = true;
	QTimer::singleShot(0, qApp, [] {
		Get().refreshQueued = false;
		Get().changes.fire({});
	});
}

QString Label(const Action &action) {
	return u"%1 · %2 [%3]"_q.arg(action.title, action.name, action.owner.left(8));
}

void Run(const QString &id, quint64 generation, const QString &source, const QString &account = {}) {
	const auto &state = Get();
	const auto i = state.entries.find(id);
	if (i == end(state.entries) || i->second.generation != generation
		|| !state.send || Core::App().passcodeLocked() || InstallationUiActive()) return;
	if (ProtectedSurfaceExists()) return;
	const auto window = Core::App().activePrimaryWindow();
	const auto controller = window ? window->sessionController() : nullptr;
	const auto active = controller ? QString::number(controller->session().uniqueId()) : QString();
	state.send({
		{ u"type"_q, u"action"_q },
		{ u"installation_id"_q, i->second.owner },
		{ u"rule_id"_q, id },
		{ u"value"_q, QJsonObject{
			{ u"source"_q, source },
			{ u"account_id"_q, account.isEmpty() ? active : account },
		} },
	});
}

QString SettingsKey(const Action &action) {
	return action.owner + '/' + action.name;
}

QSettings Settings() {
	return QSettings(cWorkingDir() + u"tdata/plugin-shortcuts.ini"_q, QSettings::IniFormat);
}

// The desktop backend serializes one uint64 descriptor per key; the order
// in which a chord was pressed must not allow two actions to claim it.
std::vector<QByteArray> Chord(const QByteArray &bytes) {
	auto result = std::vector<QByteArray>();
	for (auto offset = 0; offset < bytes.size(); offset += sizeof(uint64)) {
		result.push_back(bytes.mid(offset, sizeof(uint64)));
	}
	std::sort(result.begin(), result.end());
	return result;
}

bool Watch(const QString &id, quint64 generation, base::GlobalShortcut value) {
	auto &state = Get();
	const auto i = state.entries.find(id);
	if (i == end(state.entries) || i->second.generation != generation || !i->second.global) return false;
	auto &action = i->second;
	if (value) {
		const auto bytes = value->serialize();
		if (bytes.size() < 2 * sizeof(uint64) || bytes.size() > 4 * sizeof(uint64)
			|| bytes.size() % sizeof(uint64)) return false;
		const auto chord = Chord(bytes);
		if (chord == Chord(Core::App().settings().groupCallPushToTalkShortcut())) return false;
		for (const auto &[other, item] : state.entries) {
			if (other != id && item.binding && Chord(item.binding->serialize()) == chord) return false;
		}
	}
	if (action.binding) state.shortcuts->stopWatching(action.binding);
	action.binding = std::move(value);
	if (action.binding) {
		if (!state.shortcuts) state.shortcuts = base::CreateGlobalShortcutManager();
		state.shortcuts->startWatching(action.binding, [=](bool pressed) {
			if (pressed) crl::on_main([=] { Run(id, generation, u"global_shortcut"_q); });
		});
	}
	return true;
}

void ReleaseIdleWatcher() {
	auto &state = Get();
	if (std::none_of(state.entries.begin(), state.entries.end(), [](const auto &pair) {
		return bool(pair.second.binding);
	})) {
		state.shortcuts.reset();
	}
}

void Configure(const QString &id, quint64 generation) {
	auto &state = Get();
	const auto i = state.entries.find(id);
	const auto window = Core::App().activePrimaryWindow();
	const auto controller = window ? window->sessionController() : nullptr;
	if (i == end(state.entries) || i->second.generation != generation || !controller || !i->second.global
		|| Core::App().passcodeLocked() || InstallationUiActive()
		|| ProtectedSurfaceExists()) return;
	const auto key = SettingsKey(i->second);
	const auto label = Label(i->second);
	const auto current = i->second.binding
		? i->second.binding->toDisplayString()
		: Tr(u"Not set"_q, u"Не задано"_q);
	controller->window().activate();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		ProtectSurface(box);
		box->setWidth(style::ConvertScale(380));
		box->setTitle(rpl::single(Tr(u"Plugin keyboard shortcut"_q, u"Сочетание клавиш плагина"_q)));
		box->addTopButton(st::settingsPluginClose, [=] { box->closeBox(); });
		box->addRow(object_ptr<Ui::FlatLabel>(box, label, st::boxLabel));
		const auto field = box->addRow(object_ptr<Ui::FlatLabel>(box, current, st::boxLabel));
		// Recording is explicitly started in native UI and stops when the
		// box closes or loses focus. Plugins receive only action activation.
		const auto recorder = box->lifetime().make_state<std::unique_ptr<base::GlobalShortcutManager>>();
		box->lifetime().add([=] {
			if (*recorder) {
				(*recorder)->stopRecording();
			}
		});
		QObject::connect(qApp, &QGuiApplication::applicationStateChanged, box.get(), [=](Qt::ApplicationState s) {
			if (s != Qt::ApplicationActive && *recorder) {
				(*recorder)->stopRecording();
				field->setText(Tr(u"Recording stopped"_q, u"Запись остановлена"_q));
			}
		});
		Core::App().passcodeLockChanges() | rpl::on_next([=](bool locked) {
			if (locked) box->closeBox();
		}, box->lifetime());
		box->addButton(rpl::single(Tr(u"Record"_q, u"Записать"_q)), [=] {
			if (!Get().entries.contains(id) || Core::App().passcodeLocked()
				|| !base::GlobalShortcutsAvailable() || !base::GlobalShortcutsAllowed()) {
				field->setText(Tr(u"Global shortcuts are unavailable"_q, u"Глобальные сочетания недоступны"_q));
				return;
			}
			if (!*recorder) {
				*recorder = base::CreateGlobalShortcutManager();
			} else {
				(*recorder)->stopRecording();
			}
			field->setText(Tr(u"Press and release a key combination"_q, u"Нажмите и отпустите сочетание клавиш"_q));
			(*recorder)->startRecording(nullptr, crl::guard(box, [=](base::GlobalShortcut value) {
				if (Core::App().passcodeLocked() || !box->window()->isActiveWindow()) return;
				if (!value || !Watch(id, generation, value)) {
					field->setText(Tr(
						u"Choose an unused combination of 2–4 keys"_q,
						u"Выберите свободное сочетание из 2–4 клавиш"_q));
					return;
				}
				Settings().setValue(key, value->serialize());
				field->setText(value->toDisplayString());
			}));
		}, st::settingsPluginDialog);
		box->addLeftButton(rpl::single(Tr(u"Reset"_q, u"Сбросить"_q)), [=] {
			if (*recorder) (*recorder)->stopRecording();
			if (Watch(id, generation, nullptr)) Settings().remove(key);
			ReleaseIdleWatcher();
			box->closeBox();
		}, st::settingsPluginDialog);
		box->addButton(rpl::single(Tr(u"Close"_q, u"Закрыть"_q)), [=] { box->closeBox(); }, st::settingsPluginDialog);
	}));
}

} // namespace

void SetEventSender(std::function<void(QJsonObject)> send) {
	auto &state = Get();
	state.send = std::move(send);
	if (!state.send) {
		while (!state.entries.empty()) {
			const auto i = state.entries.begin();
			Remove(i->second.owner, i->first);
		}
		state.shortcuts.reset();
		Refresh();
	}
}

void Register(const QJsonObject &command) {
	static const auto hex = QRegularExpression(u"\\A[a-f0-9]{32}\\z"_q);
	static const auto slug = QRegularExpression(u"\\A[a-z][a-z0-9_.-]{0,47}\\z"_q);
	const auto id = command.value(u"rule_id"_q).toString();
	auto action = Action{
		.owner = command.value(u"installation_id"_q).toString(),
		.name = command.value(u"name"_q).toString(),
		.title = command.value(u"title"_q).toString().trimmed(),
		.tray = command.value(u"tray"_q).toBool(),
		.link = command.value(u"link"_q).toBool(),
		.global = command.value(u"global_shortcut"_q).toBool(),
	};
	auto &state = Get();
	if (!hex.match(id).hasMatch() || !hex.match(action.owner).hasMatch()
		|| !slug.match(action.name).hasMatch() || action.title.isEmpty() || action.title.size() > 64
		|| std::any_of(action.title.begin(), action.title.end(), [](QChar c) {
			return !c.isPrint() && !c.isSurrogate();
		})) {
		throw std::runtime_error("Invalid plugin action.");
	}
	if (state.entries.size() >= 128 || state.entries.contains(id)
		|| std::any_of(state.entries.begin(), state.entries.end(), [&](const auto &entry) {
			return entry.second.name == action.name && entry.second.owner == action.owner;
		})) throw std::runtime_error("Plugin action name is already used or limit reached.");
	const auto sequence = command.value(u"shortcut"_q).toString();
	if (!sequence.isEmpty()) {
		if (sequence.size() > 80) throw std::runtime_error("Invalid shortcut length.");
		Widgets::Handle({
			{ u"method"_q, u"ui.keys.add"_q },
			{ u"rule_id"_q, id },
			{ u"installation_id"_q, action.owner },
			{ u"sequence"_q, sequence },
			{ u"exclusive"_q, true },
		});
		action.local = true;
	}
	action.generation = ++state.nextGeneration;
	const auto i = state.entries.emplace(id, std::move(action)).first;
	if (i->second.global && base::GlobalShortcutsAvailable() && base::GlobalShortcutsAllowed()) {
		const auto bytes = Settings().value(SettingsKey(i->second)).toByteArray();
		if (bytes.size() >= 16 && bytes.size() <= 32 && bytes.size() % 8 == 0) {
			if (!state.shortcuts) state.shortcuts = base::CreateGlobalShortcutManager();
			Watch(id, i->second.generation, state.shortcuts->shortcutFromSerialized(bytes));
			ReleaseIdleWatcher();
		}
	}
	Refresh();
}

void Remove(const QString &owner, const QString &id) {
	auto &state = Get();
	const auto i = state.entries.find(id);
	if (i == end(state.entries) || i->second.owner != owner) return;
	if (i->second.binding) state.shortcuts->stopWatching(i->second.binding);
	if (i->second.local) Widgets::Handle({
		{ u"method"_q, u"ui.widgets.unrule"_q },
		{ u"rule_id"_q, id },
		{ u"installation_id"_q, owner },
	});
	state.entries.erase(i);
	ReleaseIdleWatcher();
	Refresh();
}

bool ConfigureOwned(const QString &owner, const QString &id) {
	const auto i = Get().entries.find(id);
	if (i == end(Get().entries) || i->second.owner != owner || !i->second.global
		|| Core::App().passcodeLocked() || InstallationUiActive() || ProtectedSurfaceExists()
		|| !base::GlobalShortcutsAvailable() || !base::GlobalShortcutsAllowed()) return false;
	const auto window = Core::App().activePrimaryWindow();
	if (!window || !window->sessionController()) return false;
	Configure(id, i->second.generation);
	return true;
}

void RemovePlugin(const QString &owner) {
	for (auto i = Get().entries.begin(); i != Get().entries.end();) {
		auto current = i++;
		if (current->second.owner == owner) Remove(owner, current->first);
	}
}

rpl::producer<> TrayChanges() {
	return Get().changes.events();
}

void FillTray(Platform::Tray &tray) {
	for (const auto &[key, action] : Get().entries) {
		const auto id = key;
		const auto generation = action.generation;
		if (action.tray) {
			tray.addAction(rpl::single(Label(action)), [=] { Run(id, generation, u"tray"_q); });
		}
		if (action.global && base::GlobalShortcutsAvailable()) {
			tray.addAction(rpl::single(Tr(u"Shortcut — "_q, u"Сочетание — "_q) + Label(action)), [=] { Configure(id, generation); });
		}
	}
}

void OpenLink(Window::SessionController *controller, const QString &owner, const QString &name) {
	if (!controller || Core::App().passcodeLocked()
		|| InstallationUiActive() || ProtectedSurfaceExists()) return;
	for (const auto &[key, action] : Get().entries) {
		if (!action.link || action.owner != owner || action.name != name) continue;
		const auto id = key;
		const auto generation = action.generation;
		const auto label = Label(action);
		const auto session = base::make_weak(&controller->session());
		controller->show(Box([=](not_null<Ui::GenericBox*> box) {
			ProtectSurface(box);
			box->setTitle(rpl::single(Tr(u"Run plugin action"_q, u"Вызов действия плагина"_q)));
			box->addTopButton(st::settingsPluginClose, [=] { box->closeBox(); });
			box->addRow(object_ptr<Ui::FlatLabel>(box, label, st::boxLabel));
			const auto confirmed = std::make_shared<bool>(false);
			QObject::connect(box.get(), &QObject::destroyed, qApp, [=] {
				if (!*confirmed) {
					return;
				}
				QTimer::singleShot(0, qApp, [=] {
					if (const auto strong = session.get()) {
						Run(id, generation, u"link"_q, QString::number(strong->uniqueId()));
					}
				});
			});
			box->addLeftButton(rpl::single(Tr(u"Cancel"_q, u"Отмена"_q)), [=] { box->closeBox(); }, st::settingsPluginDialog);
			box->addButton(rpl::single(Tr(u"Run"_q, u"Выполнить"_q)), [=] {
				*confirmed = true;
				box->closeBox();
			}, st::settingsPluginDialog);
		}));
		return;
	}
	controller->showToast(Tr(u"Plugin action is unavailable"_q, u"Действие плагина недоступно"_q));
}

} // namespace Ovygram::Actions
