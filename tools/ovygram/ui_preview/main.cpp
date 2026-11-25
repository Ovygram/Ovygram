#include "base/integration.h"
#include "base/parse_helper.h"
#include "base/unique_qptr.h"
#include "base/concurrent_timer.h"
#include "ovygram/plugin_install_view.h"
#include "ovygram/plugin_list_view.h"
#include "ovygram/plugin_management_view.h"
#include "ovygram/plugin_ui_tree.h"
#include "ovygram/plugin_proxy_view.h"
#include "plugin_list.h"
#include "ui/effects/animations.h"
#include "ui/emoji_config.h"
#include "ui/integration.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/layer_widget.h"
#include "ui/main_queue_processor.h"
#include "ui/style/style_core_palette.h"
#include "ui/vertical_list.h"
#include "ui/ui_utility.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/separate_panel.h"
#include "styles/style_layers.h"
#include "styles/style_plugin_management.h"

#include <QtCore/QCommandLineParser>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QPainter>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QTextEdit>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

rpl::event_stream<> UpdateRequests;

class PreviewApplication final : public QApplication {
public:
	using QApplication::QApplication;

	bool notify(QObject *object, QEvent *event) override {
		if (event->type() == QEvent::UpdateRequest) {
			UpdateRequests.fire({});
		}
		return QApplication::notify(object, event);
	}
};

class Integration final : public base::Integration, public Ui::Integration {
public:
	Integration(int argc, char **argv) : base::Integration(argc, argv) {
		base::Integration::Set(this);
		Ui::Integration::Set(this);
	}

	void enterFromEventLoop(FnMut<void()> &&call) override { call(); }
	bool logSkipDebug() override { return true; }
	void logMessageDebug(const QString &) override {}
	void logMessage(const QString &text) override { qInfo().noquote() << text; }
	void postponeCall(FnMut<void()> &&call) override {
		QTimer::singleShot(0, qApp, [call = std::move(call)]() mutable { call(); });
	}
	void registerLeaveSubscription(not_null<QWidget*>) override {}
	void unregisterLeaveSubscription(not_null<QWidget*>) override {}
	QString emojiCacheFolder() override { return _temp.filePath(u"emoji"_q); }
	QString fontsCacheFolder() override { return _temp.filePath(u"fonts"_q); }
	QString openglCheckFilePath() override { return _temp.filePath(u"opengl"_q); }
	QString angleBackendFilePath() override { return _temp.filePath(u"angle"_q); }
	void touchCounterIncrement() override { ++_touches; }
	int touchCounterNow() override { return _touches; }

private:
	QTemporaryDir _temp;
	int _touches = 0;
};

void Check(bool condition, const char *message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

void ApplyPalette(const QString &path) {
	if (path.isEmpty()) {
		return;
	}
	auto file = QFile(path);
	Check(file.open(QIODevice::ReadOnly), "Can't read the preview palette.");
	const auto source = QString::fromUtf8(base::parse::stripComments(file.readAll()));
	const auto expression = QRegularExpression(u"(\\w+)\\s*:\\s*(#[0-9a-fA-F]{6,8}|\\w+)\\s*;"_q);
	auto values = expression.globalMatch(source);
	auto palette = style::palette();
	auto count = 0;
	while (values.hasNext()) {
		const auto match = values.next();
		const auto name = match.captured(1).toLatin1();
		const auto value = match.captured(2).toLatin1();
		if (!value.startsWith('#')) {
			palette.setColor(QLatin1String(name), QLatin1String(value));
		} else {
			const auto rgb = QColor(QString::fromLatin1(value.left(7)));
			const auto alpha = value.size() == 9 ? value.mid(7).toInt(nullptr, 16) : 255;
			palette.setColor(QLatin1String(name), rgb.red(), rgb.green(), rgb.blue(), alpha);
		}
		++count;
	}
	Check(count > 0, "The preview palette has no colors.");
	palette.finalize();
	style::main_palette::apply(palette);
}

QJsonObject SamplePlugin() {
	return {
		{ u"name"_q, u"PROXY · Подписки (Windows)"_q },
		{ u"id"_q, u"proxy_windows"_q },
		{ u"plugin_id"_q, u"proxy_windows"_q },
		{ u"version"_q, u"1.0.3"_q },
		{ u"display_author"_q, u"Неизвестный автор"_q },
		{ u"description"_q,
			u"Импорт профилей PROXY из HTTPS-подписок, текста, Base64, JSON и Clash YAML. "
			u"Поиск серверов и автообновление."_q },
		{ u"release"_q, true },
		{ u"enabled"_q, true },
		{ u"loaded"_q, true },
		{ u"requirements"_q, QJsonArray{ u"pyyaml==6.0.2"_q } },
		{ u"trust"_q, QJsonObject{ { u"status"_q, u"unverified"_q } } },
	};
}

QJsonArray SamplePluginList() {
	auto subscriptions = SamplePlugin();
	subscriptions.insert(u"installation_id"_q, u"8e88c753"_q);
	subscriptions.insert(u"update"_q, QJsonObject{ { u"latest_version"_q, u"1.0.4"_q } });
	return {
		subscriptions,
		QJsonObject{
			{ u"name"_q, u"Черновик заметки"_q },
			{ u"installation_id"_q, u"1c03ee79"_q },
			{ u"plugin_id"_q, u"note_draft"_q },
			{ u"author"_q, u"Ovygram"_q },
			{ u"version"_q, u"1.0.0"_q },
			{ u"description"_q, u"Сохраняет заголовок и текст заметки в локальном черновике."_q },
			{ u"trust"_q, QJsonObject{ { u"status"_q, u"official"_q } } },
		},
		QJsonObject{
			{ u"name"_q, u"Очистка ссылок"_q },
			{ u"installation_id"_q, u"c65e509c"_q },
			{ u"plugin_id"_q, u"clean_links"_q },
			{ u"author"_q, u"Demo Studio"_q },
			{ u"version"_q, u"2.1.0"_q },
			{ u"description"_q, u"Удаляет метки отслеживания из ссылок в исходящих сообщениях."_q },
			{ u"enabled"_q, true },
			{ u"loaded"_q, true },
			{ u"trust"_q, QJsonObject{ { u"status"_q, u"verified"_q } } },
		},
	};
}

QJsonArray SampleSettings(bool longForm) {
	auto fields = QJsonArray{ QJsonObject{
		{ u"kind"_q, u"link"_q },
		{ u"key"_q, u"open"_q },
		{ u"action_id"_q, u"sample-settings"_q },
		{ u"text"_q, u"Открыть «PROXY · Подписки»"_q },
		{ u"description"_q, u"Также доступно из бокового меню над списком чатов"_q },
	} };
	if (longForm) {
		for (auto index = 1; index <= 18; ++index) {
			fields.append(QJsonObject{
				{ u"kind"_q, u"switch"_q },
				{ u"key"_q, QString::number(index) },
				{ u"text"_q, u"Настройка %1"_q.arg(index) },
				{ u"value"_q, index % 2 == 0 },
			});
		}
	}
	return fields;
}

QJsonObject InstallInfo(const QString &scenario) {
	auto metadata = SamplePlugin();
	if (scenario == u"install-long"_q) {
		metadata.insert(u"description"_q,
			(metadata.value(u"description"_q).toString() + u"\n\n"_q).repeated(12)
			+ u"Конец полного описания."_q);
	}
	auto old = metadata;
	old.insert(u"version"_q, u"1.0.2"_q);
	old.insert(u"installation_id"_q, u"8e88c753000000000000000000000001"_q);
	auto installations = QJsonArray();
	if (scenario == u"install-update"_q || scenario == u"install-multiple"_q) {
		installations.append(old);
	}
	if (scenario == u"install-multiple"_q) {
		old.insert(u"installation_id"_q, u"1c03ee79000000000000000000000002"_q);
		old.insert(u"version"_q, u"1.0.1"_q);
		installations.append(old);
	}
	return {
		{ u"metadata"_q, metadata },
		{ u"trust"_q, metadata.value(u"trust"_q) },
		{ u"sha256"_q, u"1111111111111111111111111111111111111111111111111111111111111111"_q },
		{ u"installations"_q, installations },
	};
}

// Native layer shell with the same fit-to-content behavior as the settings page.
class SettingsLayer final : public Ui::LayerWidget {
public:
	SettingsLayer(QWidget *parent, QString name, int desiredWidth)
	: Ui::LayerWidget(parent)
	, _desiredWidth(desiredWidth)
	, _title(Ui::CreateChild<Ui::FlatLabel>(this, std::move(name), st::boxTitle))
	, _back(Ui::CreateChild<Ui::IconButton>(this, st::separatePanelBack))
	, _close(Ui::CreateChild<Ui::IconButton>(this, st::boxTitleClose))
	, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll))
	, _content(_scroll->setOwnedWidget(object_ptr<Ui::VerticalLayout>(_scroll))) {
		_back->setAccessibleName(u"Назад"_q);
		_close->setAccessibleName(u"Закрыть"_q);
		_back->setClickedCallback([this] { closeLayer(); });
		_close->setClickedCallback([this] { closeLayer(); });
	}

	not_null<Ui::VerticalLayout*> content() const { return _content.data(); }
	int scrollMaximum() const { return _scroll->scrollTopMax(); }
	void setBackCallback(Fn<void()> callback) { _back->setClickedCallback(std::move(callback)); }

	void parentResized() override {
		const auto gap = style::ConvertScale(24);
		const auto width = std::min(_desiredWidth, parentWidget()->width() - 2 * gap);
		const auto head = st::boxTitleHeight;
		const auto footer = style::ConvertScale(6);
		_content->resizeToWidth(width);
		const auto height = std::min(head + _content->height() + footer, parentWidget()->height() - 2 * gap);
		resize(width, height);
		move((parentWidget()->width() - width) / 2, gap);
		_scroll->setGeometry(0, head, width, height - head - footer);
		_back->moveToLeft(0, 0, width);
		_close->moveToRight(0, 0, width);
		_title->resizeToWidth(width - 2 * head);
		_title->moveToLeft(head, (head - _title->height()) / 2, width);
	}

protected:
	void paintEvent(QPaintEvent *) override {
		auto painter = QPainter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		painter.setBrush(st::windowBg);
		painter.drawRoundedRect(rect(), st::boxRadius, st::boxRadius);
		painter.fillRect(0, st::boxTitleHeight - st::lineWidth, width(), st::lineWidth, st::shadowFg);
	}

private:
	const int _desiredWidth;
	const not_null<Ui::FlatLabel*> _title;
	const not_null<Ui::IconButton*> _back;
	const not_null<Ui::IconButton*> _close;
	const not_null<Ui::ScrollArea*> _scroll;
	const QPointer<Ui::VerticalLayout> _content;
};

QWidget *ByName(QWidget *root, const QString &name) {
	for (const auto widget : root->findChildren<QWidget*>()) {
		const auto rp = dynamic_cast<Ui::RpWidget*>(widget);
		if (rp && widget->isVisible()
			&& (rp->accessibilityName() == name || widget->accessibleName() == name)) {
			return widget;
		}
	}
	return nullptr;
}

bool Scrolls(QWidget *root) {
	for (const auto widget : root->findChildren<QWidget*>()) {
		const auto scroll = dynamic_cast<Ui::ScrollArea*>(widget);
		if (scroll && scroll->isVisible() && scroll->scrollTopMax() > 0) {
			return true;
		}
	}
	return false;
}

void Click(QWidget *root, const QString &name) {
	const auto button = ByName(root, name);
	Check(button != nullptr, "Expected control is not visible.");
	QTest::mouseClick(button, Qt::LeftButton);
	QApplication::processEvents();
}

class Preview final : public Ui::RpWidget {
public:
	Preview(QString scenario, QString output, QString action, bool check)
	: _scenario(std::move(scenario))
	, _output(std::move(output))
	, _action(std::move(action))
	, _check(check)
	, _layers(this, [] { return std::shared_ptr<Ui::Show>(); }) {
		setWindowTitle(u"Ovygram — предпросмотр"_q);
		_layers.setGeometry(rect());
	}

	~Preview() {
		for (const auto &[id, surface] : _surfaces) {
			if (surface->box) {
				disconnect(surface->box, nullptr, this, nullptr);
			}
		}
		disconnect(&_process, nullptr, this, nullptr);
		if (_process.state() != QProcess::NotRunning) {
			_process.closeWriteChannel();
			if (!_process.waitForFinished(1500)) {
				_process.kill();
				_process.waitForFinished(1000);
			}
		}
	}

	void start(const QString &plugin, const QString &menu, const QString &setting,
			const QString &python, const QString &dependencies) {
		if (!plugin.isEmpty()) {
			startPlugin(plugin, menu, setting, python, dependencies);
		} else if (_scenario == u"proxies"_q) {
			showProxies();
			finishSoon();
		} else if (_scenario.startsWith(u"install-"_q)) {
			showInstaller(InstallInfo(_scenario));
			finishSoon();
		} else if (_scenario.startsWith(u"list-"_q)) {
			_listPlugins = SamplePluginList();
			showPluginList();
			finishSoon();
		} else {
			auto sample = SamplePlugin();
			const auto enabled = _scenario != u"stopped"_q;
			sample.insert(u"enabled"_q, enabled);
			sample.insert(u"loaded"_q, enabled);
			showDetails(std::move(sample), enabled ? SampleSettings(_scenario == u"long"_q) : QJsonArray());
			finishSoon();
		}
	}

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter(this).fillRect(rect(), st::dialogsBg);
	}
	void resizeEvent(QResizeEvent *) override { _layers.setGeometry(rect()); }

private:
	void showPluginList();
	void buildPluginList();
	void showListNotice(QString title, QString body);

	void showDetails(QJsonObject plugin, QJsonArray fields) {
		_plugin = std::move(plugin);
		_fields = std::move(fields);
		auto layer = object_ptr<SettingsLayer>(&_layers, _plugin.value(u"name"_q).toString(),
			st::settingsPluginsLayerWidth);
		if (_list) {
			layer->setBackCallback([this] {
				QTimer::singleShot(0, this, [this] { showPluginList(); });
			});
		}
		_details = layer.data();
		buildDetails();
		_layers.showSpecialLayer(std::move(layer), anim::type::instant);
	}

	void buildDetails() {
		const auto layout = _details->content();
		layout->clear();
		const auto update = [this] {
			QTimer::singleShot(0, this, [this] {
				if (!_details) {
					return;
				}
				const auto enabled = !_plugin.value(u"enabled"_q).toBool();
				_plugin.insert(u"enabled"_q, enabled);
				_plugin.insert(u"loaded"_q, enabled);
				buildDetails();
			});
		};
		Ovygram::AddPluginDetailsCard(layout, _plugin, false, update);
		if (_plugin.value(u"loaded"_q).toBool() && !_fields.isEmpty()) {
			Ovygram::AddPluginSettings(layout, _fields,
				[this](QJsonValue action, bool) {
					if (_process.state() == QProcess::Running) {
						write({ { u"kind"_q, u"setting"_q }, { u"action_id"_q, action } });
					} else {
						showSampleSettings();
					}
				},
				[this](QJsonObject values) {
					if (_process.state() == QProcess::Running) {
						write({ { u"kind"_q, u"save"_q }, { u"values"_q, values } });
					}
				});
		}
		Ovygram::AddPluginManagement(layout, _plugin,
			[this] { showInstaller(InstallInfo(u"install-update"_q)); },
			[](QString) {}, [] {}, [this] { showRemoval(); });
		_details->parentResized();
	}

	void showInstaller(QJsonObject info) {
		if (!_details && !_list) {
			auto background = object_ptr<SettingsLayer>(&_layers, u"Плагины"_q,
				st::settingsPluginsLayerWidth + style::ConvertScale(36));
			const auto card = Ovygram::AddPluginCard(background->content());
			Ui::AddSkip(card, style::ConvertScale(12));
			Ovygram::AddPluginHeader(card, u"Расширения рабочего стола"_q, [] {}, true, false, [] {});
			Ovygram::AddPluginSummary(card, SamplePlugin());
			Ui::AddSkip(background->content(), style::ConvertScale(110));
			_layers.showSpecialLayer(std::move(background), anim::type::instant);
		}
		_layers.showBox(Box([=, this](not_null<Ui::GenericBox*> box) {
			const auto rows = Ovygram::PreparePluginInstallBox(box);
			const auto cancel = Ovygram::SetPluginInstallAction(box, QString(), nullptr);
			box->setFocusCallback([weak = QPointer<Ui::RoundButton>(cancel)] {
				if (weak) weak->setFocus();
			});
			const auto disclosure = box->lifetime().make_state<rpl::variable<bool>>(false);
			Ovygram::AddPluginInstallChoices(box, rows, info, {}, *disclosure,
				[this](QString id, bool separate, QJsonObject) {
					_installChoice = separate ? u"separate"_q : id.isEmpty() ? u"install"_q : id.left(8);
				},
				[=](Ui::RoundButton *button) {
					box->setFocusCallback([weak = QPointer<Ui::RoundButton>(button)] {
						if (weak) weak->setFocus();
					});
				});
		}), Ui::LayerOption::KeepOther, anim::type::instant);
	}

	void showProxies() {
		const auto state = std::make_shared<Ovygram::ProxyViewState>();
		const auto routes = QJsonArray{u"default"_q, u"api"_q, u"files"_q, u"pages"_q, u"mini_apps"_q};
		const auto offer = QJsonObject{ {u"label"_q, u"PROXY"_q}, {u"owner"_q, u"8e88c753"_q},
			{u"token"_q, u"demo"_q}, {u"routes"_q, routes} };
		state->offers.push_back(offer);
		for (const auto &value : routes) {
			state->saved.insert(value.toString(), u"8e88c753"_q);
			state->active.emplace(value.toString(), Ovygram::PluginProxyLabel(offer));
		}
		_layers.showBox(Box([=, this](not_null<Ui::GenericBox*> box) {
			Ovygram::SetupPluginProxyBox(box, std::min(style::ConvertScale(600), width() - style::ConvertScale(32)),
				[=] { return *state; }, [=](const QString &route, const QJsonObject &selected) {
					if (route.isEmpty()) {
						state->saved = QJsonObject();
						state->active.clear();
					} else if (selected.isEmpty()) {
						state->saved.remove(route);
						state->active.erase(route);
					} else {
						state->saved.insert(route, u"8e88c753"_q);
						state->active[route] = Ovygram::PluginProxyLabel(selected);
					}
				});
		}), Ui::LayerOption::KeepOther, anim::type::instant);
	}

	void showSampleSettings() {
		_layers.showBox(Box([](not_null<Ui::GenericBox*> box) {
			box->setTitle(u"PROXY · Подписки"_q);
			box->addRow(object_ptr<Ui::FlatLabel>(box, u"Тестовый раздел подписок"_q, st::boxLabel));
			box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
		}), Ui::LayerOption::KeepOther, anim::type::instant);
	}

	void showRemoval() {
		_layers.showBox(Box([](not_null<Ui::GenericBox*> box) {
			Ovygram::BuildPluginRemovalBox(box, rpl::single(u"Отмена"_q), [](bool) {});
		}), Ui::LayerOption::KeepOther, anim::type::instant);
	}

	void startPlugin(const QString &plugin, const QString &menu, const QString &setting,
		const QString &python, const QString &dependencies);
	void handlePlugin(const QJsonObject &command);
	void showPluginSurface(const QJsonObject &command);
	void write(const QJsonObject &event);
	void finishSoon();
	void verify();
	void finish();

	struct Surface {
		Ovygram::PluginUiTree tree;
		QPointer<Ui::GenericBox> box;
		std::unique_ptr<Ui::SeparatePanel> panel;
		QString owner;
	};
	const QString _scenario;
	const QString _output;
	const QString _action;
	const bool _check;
	Ui::LayerStackWidget _layers;
	QPointer<SettingsLayer> _details;
	QPointer<SettingsLayer> _list;
	Ui::InputField *_listSearch = nullptr;
	Ui::VerticalLayout *_listRows = nullptr;
	QJsonArray _listPlugins;
	bool _listLaunchBlocked = false;
	QJsonObject _plugin;
	QJsonArray _fields;
	QString _installChoice;
	QProcess _process;
	std::map<QString, std::unique_ptr<Surface>> _surfaces;
	QPointer<QWidget> _captureWindow;
	bool _updating = false;
	bool _finishQueued = false;
};

void Preview::showListNotice(QString title, QString body) {
	_layers.showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(title));
		box->addRow(object_ptr<Ui::FlatLabel>(box, body, st::boxLabel));
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
	}), Ui::LayerOption::KeepOther, anim::type::instant);
}

void Preview::showPluginList() {
	const auto before = _scenario == u"list-before"_q;
	auto layer = object_ptr<SettingsLayer>(&_layers, u"Плагины"_q, st::settingsPluginsLayerWidth);
	_list = layer.data();
	const auto layout = _list->content();
	const auto import = [this] { showInstaller(InstallInfo(u"install-new"_q)); };
	const auto check = [this] { showListNotice(u"Обновления плагинов"_q, u"Доступно одно обновление."_q); };
	if (before) {
		_listSearch = PreviewPluginList::AddSearch(layout);
		PreviewPluginList::AddActions(layout, import, check);
	} else {
		_listSearch = Ovygram::AddPluginListSearch(layout);
		Ovygram::AddPluginListActions(layout, import, check);
	}
	_listRows = layout->add(object_ptr<Ui::VerticalLayout>(layout));
	_listSearch->changes() | rpl::on_next([this] { buildPluginList(); }, _listSearch->lifetime());
	_layers.showSpecialLayer(std::move(layer), anim::type::instant);
	buildPluginList();
}

void Preview::buildPluginList() {
	_listRows->clear();
	const auto query = _listSearch->getLastText().trimmed();
	const auto open = [this](QJsonObject plugin) {
		const auto fields = plugin.value(u"plugin_id"_q) == u"proxy_windows"_q
			? SampleSettings(false) : QJsonArray();
		showDetails(std::move(plugin), fields);
	};
	const auto setEnabled = [this](QString id, bool enabled) {
		QTimer::singleShot(0, this, [=, this] {
			if (!_list) {
				return;
			}
			for (auto i = 0; i != _listPlugins.size(); ++i) {
				auto plugin = _listPlugins.at(i).toObject();
				if (plugin.value(u"installation_id"_q).toString() == id) {
					plugin.insert(u"enabled"_q, enabled);
					plugin.insert(u"loaded"_q, enabled);
					_listPlugins.replace(i, plugin);
					break;
				}
			}
			buildPluginList();
		});
	};
	const auto log = [this] { showListNotice(u"Журнал плагинов"_q, u"Журнал тестового профиля пуст."_q); };
	if (_scenario == u"list-before"_q) {
		PreviewPluginList::BuildRows(_listRows, _listPlugins, query, open, setEnabled,
			[this] { showListNotice(u"Отправить плагин"_q, u"Выбор получателя доступен в клиенте."_q); },
			[this] { showRemoval(); },
			log);
	} else {
		Ovygram::AddPluginListRows(_listRows, _listPlugins, query, _listLaunchBlocked, open, setEnabled, log);
	}
	_list->parentResized();
}

void Preview::write(const QJsonObject &event) {
	_process.write(QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n');
}

void Preview::startPlugin(
		const QString &plugin,
		const QString &menu,
		const QString &setting,
		const QString &python,
		const QString &dependencies) {
	_process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
	connect(&_process, &QProcess::readyReadStandardOutput, this, [this] {
		while (_process.canReadLine()) {
			const auto line = _process.readLine();
			try {
				handlePlugin(QJsonDocument::fromJson(line).object());
			} catch (const std::exception &error) {
				qCritical("%s", error.what());
				QCoreApplication::exit(1);
				break;
			}
		}
	});
	connect(&_process, &QProcess::errorOccurred, this, [this] {
		qCritical().noquote() << _process.errorString();
		QCoreApplication::exit(1);
	});
	connect(&_process, &QProcess::finished, this, [this](int code) {
		if (code || (!_details && _surfaces.empty())) {
			qCritical("The plugin host left the preview.");
			QCoreApplication::exit(1);
		}
	});
	_process.start(python, {
		u"-B"_q, QString::fromUtf8(OVYGRAM_PREVIEW_PYTHON),
		QFileInfo(plugin).absoluteFilePath(), menu, setting, dependencies
	});
	QTimer::singleShot(10000, this, [this] {
		if (!_details && _surfaces.empty()) {
			qCritical("No plugin window opened. Check the menu or settings key.");
			QCoreApplication::exit(1);
		}
	});
}

void Preview::handlePlugin(const QJsonObject &command) {
	const auto method = command.value(u"method"_q).toString();
	const auto id = command.value(u"object_id"_q).toString();
	if (method == u"preview.settings"_q) {
		if (!_details && _surfaces.empty()) {
			showDetails(command.value(u"plugin"_q).toObject(), command.value(u"fields"_q).toArray());
			finishSoon();
		}
	} else if (method == u"preview.error"_q) {
		throw std::runtime_error(command.value(u"message"_q).toString().toStdString());
	} else if (method == u"ui.show"_q) {
		showPluginSurface(command);
		finishSoon();
	} else if (const auto surface = _surfaces.find(id); surface != end(_surfaces)) {
		if (method == u"ui.update"_q) {
			const auto was = std::exchange(_updating, true);
			const auto restore = gsl::finally([&] { _updating = was; });
			surface->second->tree.update(command.value(u"node_id"_q).toString(),
				command.value(u"props"_q).toObject());
		} else if (method == u"ui.close"_q) {
			if (const auto box = surface->second->box) {
				box->closeBox();
			} else if (surface->second->panel) {
				const auto owner = surface->second->owner;
				_surfaces.erase(surface);
				write({ { u"type"_q, u"closed"_q }, { u"installation_id"_q, owner }, { u"object_id"_q,id } });
			}
		}
	}
}

void Preview::showPluginSurface(const QJsonObject &command) {
	const auto id = command.value(u"object_id"_q).toString();
	const auto kind = command.value(u"kind"_q).toString();
	Check(!id.isEmpty() && !_surfaces.contains(id), "Invalid plugin surface ID.");
	Check(kind == u"dialog"_q || kind == u"section"_q || kind == u"window"_q,
		"Only plugin dialogs, windows and sections are available in this preview.");
	auto owned = std::make_unique<Surface>();
	const auto surface = owned.get();
	const auto owner = surface->owner = command.value(u"installation_id"_q).toString();
	const auto title = command.value(u"title"_q).toString();
	const auto emitClosed = [this, owner, id] {
		write({ { u"type"_q, u"closed"_q }, { u"installation_id"_q, owner }, { u"object_id"_q, id } });
	};
	const auto guard = QPointer<Preview>(this);
	const auto emitEvent = [=, this](QString node, QString type, QJsonValue value) {
		if (guard && !_updating) {
			write({ { u"type"_q, type }, { u"installation_id"_q, owner },
				{ u"object_id"_q, id }, { u"node_id"_q, node }, { u"value"_q, value } });
		}
	};
	auto content = surface->tree.build(nullptr, command.value(u"tree"_q).toObject(), emitEvent);
	const auto footer = command.value(u"footer_action"_q).toObject();
	auto action = object_ptr<Ui::RpWidget>(nullptr);
	if (kind == u"dialog"_q && !footer.isEmpty()) {
		Check(footer.value(u"type"_q).toString() == u"button"_q, "A dialog footer action must be a button.");
		action = surface->tree.build(nullptr, footer, emitEvent);
	}
	_surfaces.emplace(id, std::move(owned));
	const auto requested = command.value(u"width"_q).toInt();
	_captureWindow = nullptr;
	if (kind == u"window"_q) {
		surface->panel = std::make_unique<Ui::SeparatePanel>();
		const auto panel = surface->panel.get();
		panel->setWindowFlag(Qt::WindowStaysOnTopHint, false);
		panel->setTitle(rpl::single(title));
		panel->setInnerSize(QSize(std::clamp(requested ? requested : 420, 200, 2000),
			std::clamp(command.value(u"height"_q).toInt(480), 120, 2000)), true);
		auto inner = base::make_unique_q<Ui::RpWidget>(panel);
		const auto scroll = Ui::CreateChild<Ui::ScrollArea>(inner.get(), st::boxScroll);
		inner->sizeValue() | rpl::on_next([=](QSize size) {
			scroll->setGeometry(QRect(QPoint(), size));
		}, inner->lifetime());
		const auto rows = scroll->setOwnedWidget(object_ptr<Ui::VerticalLayout>(scroll));
		if (const auto cap = command.value(u"max_content_width"_q).toInt(); cap > 0) {
			content = Ovygram::PluginUiTree::CenterContent(rows, std::move(content),
				style::ConvertScale(8), cap);
		}
		rows->add(std::move(content), st::boxRowPadding, style::al_justify);
		scroll->widthValue() | rpl::on_next([=](int width) {
			rows->resizeToWidth(width);
		}, rows->lifetime());
		panel->showInner(std::move(inner));
		panel->closeRequests() | rpl::on_next([=, this] {
			QTimer::singleShot(0, this, [=, this] { _surfaces.erase(id); emitClosed(); });
		}, panel->lifetime());
		panel->showAndActivate();
		// The panel takes ownership of inner; capture its top-level widget.
		_captureWindow = scroll->window();
	} else {
		auto wrapper = std::make_shared<object_ptr<Ui::RpWidget>>(std::move(content));
		auto footerAction = std::make_shared<object_ptr<Ui::RpWidget>>(std::move(action));
		_layers.showBox(Box([=, this](not_null<Ui::GenericBox*> box) {
			box->setTitle(rpl::single(title));
			const auto available = std::max(width() - style::ConvertScale(32), 1);
			const auto defaultWidth = (kind == u"section"_q) ? st::settingsPluginsLayerWidth : st::boxWidth;
			box->setWidth(std::min(available, requested ? style::ConvertScale(std::clamp(requested, 320, 960)) : defaultWidth));
			box->setMaxHeight(height() - style::ConvertScale(118));
			box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); });
			if (const auto cap = command.value(u"max_content_width"_q).toInt(); cap > 0) {
				*wrapper = Ovygram::PluginUiTree::CenterContent(box, std::move(*wrapper), 0, cap);
			}
			box->addRow(std::move(*wrapper), style::al_justify);
			Ovygram::PluginUiTree::AddDialogButtons(box, std::move(*footerAction));
			surface->box = box.get();
			connect(box.get(), &QObject::destroyed, this, [=, this] {
				_surfaces.erase(id);
				emitClosed();
			});
		}), Ui::LayerOption::KeepOther, anim::type::instant);
	}
}

void Preview::finishSoon() {
	if (_finishQueued || (_output.isEmpty() && !_check && _action.isEmpty())) {
		return;
	}
	_finishQueued = true;
	QTimer::singleShot(80, this, [this] {
		try {
			if (!_action.isEmpty()) {
				Click(_captureWindow ? _captureWindow.data() : this, _action);
			}
			if (_check && _process.state() != QProcess::Running) {
				verify();
			}
			QTimer::singleShot(_process.state() == QProcess::Running ? 350 : 220,
				this, [this] { finish(); });
		} catch (const std::exception &error) {
			qCritical("%s", error.what());
			QCoreApplication::exit(1);
		}
	});
}

void Preview::verify() {
	if (_scenario == u"proxies"_q) {
		const auto reset = dynamic_cast<Ui::RoundButton*>(ByName(this, u"Сбросить маршрут: Запросы Telegram"_q));
		Check(reset && !reset->isDisabled(), "The assigned route cannot be reset.");
		Click(this, u"Сбросить маршрут: Запросы Telegram"_q);
		Check(reset->isDisabled(), "Reset did not restore the default route.");
		Click(this, u"Сбросить все"_q);
		const auto defaultReset = dynamic_cast<Ui::RoundButton*>(ByName(this, u"Сбросить маршрут: Прокси по умолчанию"_q));
		Check(defaultReset && defaultReset->isDisabled(), "Reset all did not clear the routes.");
	} else if (_scenario == u"list-after"_q) {
		const auto first = u"PROXY · Подписки (Windows)"_q;
		const auto note = u"Черновик заметки"_q;
		Check(ByName(_list, first), "A plugin is missing from the list.");
		Check(ByName(_list, u"Не проверен"_q) && ByName(_list, u"Официальный"_q)
			&& ByName(_list, u"Проверен"_q), "A plugin trust badge is missing.");
		Check(ByName(_list, u"Доступно обновление до 1.0.4"_q), "Available update is missing.");
		_listLaunchBlocked = true;
		buildPluginList();
		const auto start = dynamic_cast<Ui::SettingsButton*>(ByName(_list, u"Включить Черновик заметки"_q));
		const auto stop = dynamic_cast<Ui::SettingsButton*>(ByName(_list, u"Отключить PROXY · Подписки (Windows)"_q));
		Check(start && start->isDisabled() && stop && !stop->isDisabled(),
			"Runtime recovery must prevent starting plugins and allow stopping them.");
		_listLaunchBlocked = false;
		buildPluginList();
		_listSearch->setFocus();
		const auto edit = _listSearch->rawTextEdit().get();
		QTest::keyClicks(edit, "note_draft");
		Check(!ByName(_list, first) && ByName(_list, note), "Plugin ID search did not filter the list.");
		QTest::keyClick(edit, Qt::Key_A, Qt::ControlModifier);
		QTest::keyClicks(edit, "missing");
		Check(ByName(_list, u"Плагины не найдены."_q), "Empty search result is missing.");
		Click(_list, u"Очистить поиск"_q);
		Check(ByName(_list, first), "Clearing search did not restore the list.");
		Click(_list, u"Включить Черновик заметки"_q);
		Check(ByName(_list, u"Отключить Черновик заметки"_q), "The plugin did not start.");
		Click(_list, u"Отключить Черновик заметки"_q);
		Check(ByName(_list, u"Включить Черновик заметки"_q), "The plugin did not stop.");
		Click(_list, u"Проверить обновления"_q);
		Check(ByName(this, u"Доступно одно обновление."_q), "Update feedback did not open.");
		Click(const_cast<Ui::LayerWidget*>(_layers.topShownLayer()), u"Закрыть"_q);
		_layers.finishAnimating();
		Click(_list, first);
		Check(_details && ByName(_details, u"Удалить плагин"_q), "Plugin details did not open.");
		Click(_details, u"Назад"_q);
		Check(_list && ByName(_list, first), "Back did not return to the plugin list.");
	} else if (_scenario == u"list-before"_q) {
		Check(ByName(_list, u"Отправить"_q) && ByName(_list, u"Удалить"_q),
			"The original card actions are missing.");
	} else if (_scenario.startsWith(u"install-"_q)) {
		const auto first = _scenario == u"install-new"_q || _scenario == u"install-long"_q;
		Check(bool(ByName(this, u"Новая версия"_q)) == !first, "Unexpected version heading.");
		Check(ByName(this, u"Закрыть"_q), "Installer has no close control.");
		Check(bool(Scrolls(this)) == (_scenario == u"install-long"_q), "Unexpected installer scrolling.");
		if (!first) {
			Check(ByName(this, u"Установить отдельно"_q), "Secondary install action is missing.");
			Click(this, u"Обновить"_q);
			Check(_installChoice == u"8e88c753"_q, "Update chose the wrong instance.");
		} else {
			Click(this, u"Установить"_q);
			Check(_installChoice == u"install"_q, "Fresh install chose the wrong action.");
		}
		if (_action.isEmpty()) {
			Click(this, u"Подробности файла"_q);
			Check(ByName(this, u"ID: proxy_windows"_q), "File details didn't expand.");
			Click(this, u"Подробности файла"_q);
		}
	} else {
		const auto enabled = _scenario != u"stopped"_q;
		Check(_details && (_details->scrollMaximum() > 0) == (_scenario == u"long"_q), "Unexpected settings scrolling.");
		if (enabled) {
			Click(this, u"Отключить плагин"_q);
			Check(ByName(this, u"Остановлен"_q), "The stop status didn't change.");
			Click(this, u"Включить плагин"_q);
			Check(ByName(this, u"Запущен"_q), "The start status didn't change.");
			Click(this, u"Открыть «PROXY · Подписки»"_q);
			Check(ByName(this, u"Тестовый раздел подписок"_q), "Settings destination did not open.");
			const auto top = const_cast<Ui::LayerWidget*>(_layers.topShownLayer());
			Click(top, u"Закрыть"_q);
			_layers.finishAnimating();
		}
	}
	qInfo("UI checks passed.");
}

void Preview::finish() {
	if (!_output.isEmpty()) {
		auto area = QRect();
		if (const auto top = _layers.topShownLayer()) {
			const auto topLeft = top->mapTo(this, QPoint());
			const auto padding = style::ConvertScale(22);
			area = QRect(topLeft, top->size()).adjusted(-padding, -padding, padding, padding).intersected(rect());
		}
		const auto pixmap = _captureWindow ? _captureWindow->grab() : area.isEmpty() ? grab() : grab(area);
		if (pixmap.isNull() || !pixmap.save(_output)) {
			qCritical().noquote() << "Could not save screenshot to" << _output;
			QCoreApplication::exit(1);
			return;
		}
		qInfo().noquote() << _output << pixmap.width() << "×" << pixmap.height();
	}
	// A separate SDK panel handles Close itself, so it can veto QApplication's quit event.
	QCoreApplication::exit(0);
}

} // namespace

namespace crl {

rpl::producer<> on_main_update_requests() {
	return UpdateRequests.events();
}

} // namespace crl

int main(int argc, char **argv) {
	if (qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
		&& qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
		qputenv("QT_QPA_PLATFORM", "offscreen");
	}
	auto app = PreviewApplication(argc, argv);
	app.setApplicationName(u"Ovygram UI preview"_q);
	auto parser = QCommandLineParser();
	parser.setApplicationDescription(u"Open Ovygram UI with a temporary profile and sample data."_q);
	parser.addHelpOption();
	parser.addOptions({
		{ u"scenario"_q, u"list-before, list-after, details, stopped, long, install-new, install-update, install-multiple, install-long, proxies."_q,
			u"name"_q, u"details"_q },
		{ u"palette"_q, u"Apply a plain Telegram color palette."_q, u"file"_q },
		{ u"scale"_q, u"Ovygram UI scale in percent."_q, u"percent"_q, u"100"_q },
		{ u"width"_q, u"Preview desktop width."_q, u"pixels"_q, u"820"_q },
		{ u"height"_q, u"Preview desktop height."_q, u"pixels"_q, u"620"_q },
		{ u"screenshot"_q, u"Save a PNG and exit."_q, u"file"_q },
		{ u"click"_q, u"Activate a control by its accessible name before capture."_q, u"name"_q },
		{ u"check"_q, u"Exercise the selected built-in scenario and exit."_q },
		{ u"plugin-file"_q, u"Run a plugin package in the isolated preview host."_q, u"file"_q },
		{ u"menu"_q, u"Open a main-menu item from the plugin."_q, u"label"_q },
		{ u"setting"_q, u"Open a Button or Link by its settings key."_q, u"key"_q },
		{ u"dependencies"_q, u"Prepared plugin libraries installed into a directory."_q, u"directory"_q },
		{ u"python"_q, u"System CPython 3.13 for the SDK host."_q, u"executable"_q, u"python3.13"_q },
	});
	parser.process(app);
	try {
		const auto scenario = parser.value(u"scenario"_q);
		const auto plugin = parser.value(u"plugin-file"_q);
		Check(QStringList{ u"list-before"_q, u"list-after"_q, u"details"_q, u"stopped"_q, u"long"_q, u"install-new"_q, u"install-update"_q,
			u"install-multiple"_q, u"install-long"_q, u"proxies"_q }.contains(scenario), "Unknown preview scenario.");
		Check(!(parser.isSet(u"menu"_q) && parser.isSet(u"setting"_q)), "Choose --menu or --setting.");
		Check(!plugin.isEmpty() || !(parser.isSet(u"menu"_q) || parser.isSet(u"setting"_q)),
			"Use --plugin with --menu or --setting.");
		Check(!parser.isSet(u"check"_q) || plugin.isEmpty(), "--check applies to the built-in scenarios.");
		if (app.platformName() == u"offscreen"_q) {
			Check(parser.isSet(u"screenshot"_q) || parser.isSet(u"check"_q) || parser.isSet(u"click"_q),
				"Use --screenshot or --check when no graphical display is available.");
		}
		auto integration = Integration(argc, argv);
		auto queue = Ui::MainQueueProcessor();
		auto concurrentTimers = base::ConcurrentTimerEnvironment();
		auto animations = Ui::Animations::Manager();
		const auto ratio = std::clamp(int(std::ceil(app.devicePixelRatio())), 1, 3);
		style::SetDevicePixelRatio(ratio);
		const auto scale = parser.value(u"scale"_q).toInt();
		Check(scale >= style::kScaleMin && scale <= style::MaxScaleForRatio(ratio), "UI scale is unsupported.");
		style::SetScale(scale);
		style::StartManager(scale);
		ApplyPalette(parser.value(u"palette"_q));
		Ui::Emoji::Init();
		auto preview = Preview(scenario, parser.value(u"screenshot"_q), parser.value(u"click"_q), parser.isSet(u"check"_q));
		preview.resize(std::clamp(parser.value(u"width"_q).toInt(), 420, 2400),
			std::clamp(parser.value(u"height"_q).toInt(), 360, 1800));
		preview.show();
		preview.start(plugin, parser.value(u"menu"_q), parser.value(u"setting"_q), parser.value(u"python"_q),
			parser.value(u"dependencies"_q));
		return app.exec();
	} catch (const std::exception &error) {
		qCritical("%s", error.what());
		return 1;
	}
}
