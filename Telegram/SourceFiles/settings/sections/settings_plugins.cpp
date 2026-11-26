#include "settings/sections/settings_plugins.h"

#include "core/application.h"
#include "core/file_utilities.h"
#include "core/plugin_runtime.h"
#include "ovygram/plugin_install.h"
#include "ovygram/plugin_list_view.h"
#include "ovygram/plugin_management_view.h"
#include "ovygram/protected_ui.h"
#include "lang/lang_keys.h"
#include "mainwidget.h"
#include "settings/settings_common_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>

namespace Settings {
namespace {

// How often a page refreshes while plugin libraries are installed.
constexpr auto kDependencyPoll = crl::time(1500);
// .ovyplugin and .ovyzip are the plugin formats; the rest import locally.
const auto kPluginFilter = u"Плагины (*.ovyplugin *.ovyzip *.py *.plugin *.zip)"_q;

[[nodiscard]] QJsonValue Request(
		QJsonObject request,
		rpl::variable<QString> &notice) {
	auto error = u"Не удалось запустить встроенный Python. Подробности в журнале клиента."_q;
	if (const auto runtime = Core::App().pluginRuntime()) {
		try {
			const auto response = runtime->dispatch(
				QJsonDocument(request).toJson(QJsonDocument::Compact).toStdString());
			const auto object = QJsonDocument::fromJson(
				QByteArray::fromStdString(response)).object();
			if (object.contains(u"result"_q)) {
				return object.value(u"result"_q);
			}
			error = object.value(u"error"_q).toString(u"Не удалось выполнить действие."_q);
		} catch (const std::exception &exception) {
			error = QString::fromUtf8(exception.what());
		}
	}
	notice = std::move(error);
	return QJsonValue(QJsonValue::Undefined);
}

[[nodiscard]] QString RuntimeStatus(const QJsonObject &snapshot) {
	if (snapshot.value(u"disabled"_q).toBool()) {
		return u"Плагины отключены параметром --disable-plugins."_q;
	} else if (snapshot.value(u"recovery"_q).toBool()) {
		return u"Предыдущий сеанс завершился аварийно. Плагины пока не запущены."_q;
	} else if (snapshot.value(u"restart_required"_q).toBool()) {
		return u"Для дальнейшего запуска плагинов перезапусти клиент."_q;
	}
	return {};
}

not_null<Ui::SettingsButton*> AddAction(
		not_null<Ui::VerticalLayout*> layout,
		QString text,
		Fn<void()> callback,
		const style::SettingsButton &style = st::settingsButtonNoIcon) {
	const auto button = layout->add(object_ptr<Ui::SettingsButton>(
		layout,
		rpl::single(std::move(text)),
		style));
	button->setClickedCallback(std::move(callback));
	return button;
}

void Clear(not_null<Ui::VerticalLayout*> layout) {
	while (layout->count()) {
		delete layout->widgetAt(0);
	}
}

// Keeps the height of a layout while it is rebuilt. An empty layout for a
// moment shrinks the page, and the settings scroll jumps to the top, e.g.
// after toggling a plugin far down the list.
[[nodiscard]] auto HoldHeight(not_null<Ui::VerticalLayout*> layout) {
	layout->setMinimumHeight(layout->height());
	return gsl::finally([=] {
		layout->setMinimumHeight(0);
		layout->resizeToWidth(layout->width());
	});
}

void AddNotice(
		not_null<Ui::VerticalLayout*> layout,
		rpl::variable<QString> &notice) {
	const auto wrap = layout->add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		layout,
		object_ptr<Ui::VerticalLayout>(layout)));
	Ui::AddDividerText(wrap->entity(), notice.value());
	wrap->toggleOn(notice.value() | rpl::map([](const QString &text) {
		return !text.isEmpty();
	}));
	wrap->finishAnimating();
}

[[nodiscard]] bool InstallingDependencies(const QJsonObject &snapshot) {
	return !snapshot.value(u"dependencies"_q).toObject().value(
		u"installing"_q).toString().isEmpty();
}

[[nodiscard]] QString DependencyStatus(const QJsonObject &snapshot) {
	return snapshot.value(u"dependencies"_q).toObject().value(u"restart"_q).toBool()
		? u"Новые версии библиотек плагинов применятся после перезапуска клиента."_q
		: QString();
}

// What the user must know about the process plugins run in: why it can't
// start, that it has no network, or that it isn't restricted at all.
[[nodiscard]] QString IsolationStatus(const QJsonObject &snapshot) {
	const auto isolation = snapshot.value(u"isolation"_q).toObject();
	const auto reason = isolation.value(u"reason"_q).toString().simplified();
	if (isolation.value(u"active"_q).toBool()) {
		return reason;
	} else if (isolation.value(u"running"_q).toBool() || reason.isEmpty()) {
		return reason.isEmpty()
			? QString()
			: u"Плагины работают без изоляции от системы. %1"_q.arg(reason);
	}
	return u"Плагины не запущены. %1"_q.arg(reason);
}

void ConfirmRemove(
		not_null<Window::SessionController*> controller,
		Fn<void(bool)> remove) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		Ovygram::ProtectSurface(box);
		Ovygram::BuildPluginRemovalBox(box, tr::lng_cancel(), remove);
	}));
}

void SharePlugin(
		not_null<Window::SessionController*> controller,
		const QString &id,
		rpl::variable<QString> &notice) {
	const auto path = Request({
		{ u"action"_q, u"share_file"_q },
		{ u"installation_id"_q, id },
	}, notice).toString();
	if (!path.isEmpty()) {
		notice = QString();
		controller->content()->handleStartFiles({ path });
	}
}

class PluginDetails final : public AbstractSection {
public:
	PluginDetails(
		QWidget *parent,
		not_null<Window::SessionController*> controller,
		QString installationId,
		QString name);

	[[nodiscard]] Type id() const override;
	[[nodiscard]] rpl::producer<QString> title() override;
	[[nodiscard]] rpl::producer<> sectionShowBack() override;
	[[nodiscard]] static Type Id(QString installationId, QString name);

private:
	void refresh();
	void operate(const QString &action, bool keepData = false);
	void installUpdate();
	void addSettings(const QJsonArray &fields);
	void pollWhileInstalling(const QJsonObject &snapshot);

	const QString _installationId;
	const QString _name;
	const not_null<Ui::VerticalLayout*> _content;
	rpl::variable<QString> _notice;
	rpl::event_stream<> _back;
	bool _polling = false;
};

struct DetailsFactory final : AbstractSectionFactory {
	DetailsFactory(QString installationId, QString name)
	: installationId(std::move(installationId)), name(std::move(name)) {
	}

	object_ptr<AbstractSection> create(
			not_null<QWidget*> parent,
			not_null<Window::SessionController*> controller,
			not_null<Ui::ScrollArea*> scroll,
			rpl::producer<Container> containerValue) const override {
		return object_ptr<PluginDetails>(parent, controller, installationId, name);
	}

	const QString installationId;
	const QString name;
};

PluginDetails::PluginDetails(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	QString installationId,
	QString name)
: AbstractSection(parent, controller)
, _installationId(std::move(installationId))
, _name(std::move(name))
, _content(Ui::CreateChild<Ui::VerticalLayout>(this)) {
	Ovygram::ProtectSurface(this);
	setNavigationAnchor(_content);
	Ui::ResizeFitChild(this, _content);
	refresh();
}

Type PluginDetails::Id(QString installationId, QString name) {
	return std::make_shared<DetailsFactory>(std::move(installationId), std::move(name));
}

Type PluginDetails::id() const {
	return Id(_installationId, _name);
}

rpl::producer<QString> PluginDetails::title() {
	return rpl::single(_name);
}

rpl::producer<> PluginDetails::sectionShowBack() {
	return _back.events();
}

void PluginDetails::refresh() {
	const auto hold = HoldHeight(_content);
	Clear(_content);
	AddNotice(_content, _notice);
	const auto result = Request({ { u"action"_q, u"snapshot"_q } }, _notice);
	if (result.isUndefined()) {
		return;
	}
	const auto snapshot = result.toObject();
	const auto entries = snapshot.value(u"plugins"_q).toArray();
	auto plugin = QJsonObject();
	for (const auto &entry : entries) {
		const auto object = entry.toObject();
		if (object.value(u"installation_id"_q).toString() == _installationId) {
			plugin = object;
			break;
		}
	}
	if (plugin.isEmpty()) {
		_notice = u"Плагин удалён."_q;
		return;
	}

	const auto status = RuntimeStatus(snapshot);
	const auto enabled = plugin.value(u"enabled"_q).toBool();
	Ovygram::AddPluginDetailsCard(_content, plugin,
		!status.isEmpty() || plugin.value(u"trust"_q).toObject().value(u"status"_q).toString() == u"blocked"_q,
		[=, this] { operate(enabled ? u"disable"_q : u"enable"_q); });
	if (const auto error = plugin.value(u"error"_q).toString(); !error.isEmpty()) {
		Ui::AddDividerText(_content, rpl::single(Ovygram::PluginErrorTitle(plugin) + error));
	}
	if (!status.isEmpty()) {
		Ui::AddDividerText(_content, rpl::single(status));
	}
	if (const auto text = DependencyStatus(snapshot); !text.isEmpty()) {
		Ui::AddDividerText(_content, rpl::single(text));
	}
	pollWhileInstalling(snapshot);

	if (plugin.value(u"loaded"_q).toBool()) {
		const auto fields = Request({
			{ u"action"_q, u"settings"_q },
			{ u"installation_id"_q, _installationId },
		}, _notice);
		if (fields.isArray() && !fields.toArray().isEmpty()) {
			addSettings(fields.toArray());
		}
	}

	Ovygram::AddPluginManagement(_content, plugin,
		[this] { installUpdate(); },
		[this](QString action) { operate(action); },
		[this] { SharePlugin(controller(), _installationId, _notice); },
		[this] {
			ConfirmRemove(controller(), crl::guard(this, [this](bool keepData) {
				operate(u"remove"_q, keepData);
			}));
		});
}

void PluginDetails::pollWhileInstalling(const QJsonObject &snapshot) {
	if ((InstallingDependencies(snapshot) || snapshot.value(u"distribution"_q).toObject().value(u"busy"_q).toBool()) && !_polling) {
		_polling = true;
		QTimer::singleShot(kDependencyPoll, this, [this] {
			_polling = false;
			refresh();
		});
	}
}

void PluginDetails::operate(const QString &action, bool keepData) {
	const auto result = Request([&] {
		auto request = QJsonObject{ { u"action"_q, action }, { u"installation_id"_q, _installationId } };
		if (action == u"remove"_q) request.insert(u"keep_data"_q, keepData);
		return request;
	}(), _notice);
	const auto succeeded = !result.isUndefined();
	QTimer::singleShot(0, this, [=, this] {
		if (succeeded && action == u"remove"_q) {
			_back.fire({});
		} else {
			if (succeeded) {
				_notice = QString();
			}
			refresh();
		}
	});
}

void PluginDetails::installUpdate() {
	FileDialog::GetOpenPath(this, u"Выбрать файл плагина"_q, kPluginFilter,
		crl::guard(this, [this](const FileDialog::OpenResult &files) {
			if (!files.paths.isEmpty()) {
				Ovygram::ShowPluginInstall(files.paths.front(), _installationId,
					crl::guard(this, [this] { refresh(); }));
			}
		}));
}

void PluginDetails::addSettings(const QJsonArray &fields) {
	Ovygram::AddPluginSettings(_content, fields,
		[this](QJsonValue actionId, bool link) {
			if (link) {
				const auto installationId = _installationId;
				auto &window = controller()->window();
				const auto show = controller()->uiShow();
				if (window.isLayerShown()) {
					window.hideSettingsAndLayer(anim::type::instant);
				} else {
					controller()->clearSectionStack({
						Window::SectionShow::Way::ClearStack,
						anim::type::instant,
					});
				}
				QTimer::singleShot(0, QCoreApplication::instance(), [=] {
					if (const auto runtime = Core::App().pluginRuntime()) {
						try {
							const auto response = runtime->dispatch(
								QJsonDocument(QJsonObject{
									{ u"action"_q, u"settings_action"_q },
									{ u"installation_id"_q, installationId },
									{ u"action_id"_q, actionId },
								}).toJson(QJsonDocument::Compact).toStdString());
							const auto object = QJsonDocument::fromJson(
								QByteArray::fromStdString(response)).object();
							if (!object.contains(u"result"_q) && show && show->valid()) {
								show->showToast(object.value(u"error"_q).toString(
									u"Не удалось выполнить действие."_q));
							}
						} catch (const std::exception &exception) {
							if (show && show->valid()) {
								show->showToast(QString::fromUtf8(exception.what()));
							}
						}
					}
				});
				return;
			}
			(void)Request({ { u"action"_q, u"settings_action"_q },
				{ u"installation_id"_q, _installationId },
				{ u"action_id"_q, actionId } }, _notice);
			QTimer::singleShot(0, this, [this] { refresh(); });
		},
		[this](QJsonObject values) {
			if (!Request({
				{ u"action"_q, u"save_settings"_q },
				{ u"installation_id"_q, _installationId },
				{ u"values"_q, values },
			}, _notice).isUndefined()) {
				_notice = u"Настройки сохранены."_q;
			}
		});
}

class Plugins final : public Section<Plugins> {
public:
	Plugins(QWidget *parent, not_null<Window::SessionController*> controller);

	[[nodiscard]] rpl::producer<QString> title() override;
	void showFinished() override;

private:
	void refresh();
	void populate();
	void install();
	void setPluginEnabled(const QString &id, bool enabled);
	void applyUpdate(const QString &id);

	Ui::InputField *_search = nullptr;
	Ui::VerticalLayout *_content = nullptr;
	Ui::RoundButton *_checkUpdates = nullptr;
	QJsonObject _snapshot;
	rpl::variable<QString> _notice;
	bool _polling = false;
	bool _manualCheckPending = false;
	bool _shownOnce = false;
};

Plugins::Plugins(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: Section(parent, controller) {
	Ovygram::ProtectSurface(this);
	const auto layout = Ui::CreateChild<Ui::VerticalLayout>(this);
	_search = Ovygram::AddPluginListSearch(layout);
	setNavigationAnchor(layout);
	_checkUpdates = Ovygram::AddPluginListActions(layout, [this] { install(); }, [this] {
		const auto started = Request({ { u"action"_q, u"check_updates"_q } }, _notice);
		if (started.isUndefined()) return;
		_manualCheckPending = true;
		_notice = started.toBool() ? u"Проверяю обновления плагинов…"_q : u"Проверка уже выполняется…"_q;
		refresh();
	});
	Ovygram::AddPluginListNotice(layout, _notice.value());
	_content = layout->add(object_ptr<Ui::VerticalLayout>(layout));
	Ui::ResizeFitChild(this, layout);

	_search->changes() | rpl::on_next([this] { populate(); }, layout->lifetime());
	refresh();
}

rpl::producer<QString> Plugins::title() {
	return rpl::single(u"Плагины"_q);
}

void Plugins::showFinished() {
	Section<Plugins>::showFinished();
	if (std::exchange(_shownOnce, true)) {
		refresh();
	}
}

void Plugins::refresh() {
	const auto result = Request({ { u"action"_q, u"snapshot"_q } }, _notice);
	if (!result.isObject()) {
		return;
	}
	_snapshot = result.toObject();
	const auto distribution = _snapshot.value(u"distribution"_q).toObject();
	_checkUpdates->setDisabled(distribution.value(u"checking"_q).toBool());
	if (_manualCheckPending && !distribution.value(u"checking"_q).toBool()
		&& distribution.value(u"last_check"_q).isObject()) {
		_manualCheckPending = false;
		const auto result = distribution.value(u"last_check"_q).toObject();
		const auto errors = result.value(u"errors"_q).toArray();
		const auto updates = result.value(u"updates"_q).toInt();
		_notice = updates
			? u"Найдены новые или безопасные версии: %1."_q.arg(updates)
			: result.value(u"checked"_q).toInt()
			? u"Проверка завершена. Новых проверенных версий нет."_q
			: u"Установленных плагинов для проверки нет."_q;
		if (!errors.isEmpty()) {
			_notice = u"Проверка завершена с ошибками. "_q
				+ (updates ? u"Доступно обновлений: %1. "_q.arg(updates) : QString())
				+ errors.first().toString();
		}
	}
	populate();
	// The plugin starts when its libraries are installed in the background.
	if ((InstallingDependencies(_snapshot) || _snapshot.value(u"distribution"_q).toObject().value(u"busy"_q).toBool()) && !_polling) {
		_polling = true;
		QTimer::singleShot(kDependencyPoll, this, [this] {
			_polling = false;
			refresh();
		});
	}
}

void Plugins::populate() {
	const auto hold = HoldHeight(_content);
	Clear(_content);
	if (_snapshot.isEmpty()) {
		return;
	}
	for (const auto &notice : _snapshot.value(u"distribution"_q).toObject().value(u"notices"_q).toArray()) {
		Ovygram::AddPluginListNotice(_content, rpl::single(notice.toString()));
	}
	const auto status = RuntimeStatus(_snapshot);
	if (!status.isEmpty()) {
		Ovygram::AddPluginListNotice(_content, rpl::single(status));
	}
	if (const auto text = DependencyStatus(_snapshot); !text.isEmpty()) {
		Ovygram::AddPluginListNotice(_content, rpl::single(text));
	}
	if (const auto text = IsolationStatus(_snapshot); !text.isEmpty()) {
		Ovygram::AddPluginListNotice(_content, rpl::single(text));
	}
	if (_snapshot.value(u"recovery"_q).toBool()
		&& !_snapshot.value(u"disabled"_q).toBool()
		&& !_snapshot.value(u"restart_required"_q).toBool()) {
		AddAction(_content, u"Возобновить запуск плагинов"_q, [this] {
			_notice = QString();
			(void)Request({ { u"action"_q, u"resume"_q } }, _notice);
			crl::on_main(this, [this] { refresh(); });
		});
	}
	Ovygram::AddPluginListRows(
		_content,
		_snapshot.value(u"plugins"_q).toArray(),
		_search->getLastText().trimmed(),
		!status.isEmpty(),
		[this](QJsonObject plugin) {
			showOther(PluginDetails::Id(
				plugin.value(u"installation_id"_q).toString(),
				plugin.value(u"name"_q).toString()));
		},
		[this](QString id, bool enabled) { setPluginEnabled(id, enabled); },
		[this](QString id) { applyUpdate(id); },
		[this] {
			const auto path = _snapshot.value(u"log_path"_q).toString();
			if (path.isEmpty() || !QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
				_notice = u"Не удалось открыть журнал плагинов."_q;
			}
		});
}

void Plugins::setPluginEnabled(const QString &id, bool enabled) {
	_notice = QString();
	(void)Request({
		{ u"action"_q, enabled ? u"enable"_q : u"disable"_q },
		{ u"installation_id"_q, id },
	}, _notice);
	QTimer::singleShot(0, this, [this] { refresh(); });
}

void Plugins::applyUpdate(const QString &id) {
	_notice = QString();
	(void)Request({
		{ u"action"_q, u"apply_update"_q },
		{ u"installation_id"_q, id },
	}, _notice);
	QTimer::singleShot(0, this, [this] { refresh(); });
}

void Plugins::install() {
	FileDialog::GetOpenPath(this, u"Выбрать файл плагина"_q, kPluginFilter,
		crl::guard(this, [this](const FileDialog::OpenResult &files) {
			if (!files.paths.isEmpty()) {
				Ovygram::ShowPluginInstall(files.paths.front(), QString(),
					crl::guard(this, [this] { refresh(); }));
			}
		}));
}

} // namespace

Type PluginsId() {
	return Plugins::Id();
}

bool IsPluginsSection(const Type &type) {
	return type == Plugins::Id()
		|| dynamic_cast<const DetailsFactory*>(type.get()) != nullptr;
}

} // namespace Settings
