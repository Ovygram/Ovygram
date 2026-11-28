#include "base/integration.h"
#include "base/parse_helper.h"
#include "base/unique_qptr.h"
#include "base/concurrent_timer.h"
#include "ovygram/auto_update_view.h"
#include "ovygram/i18n.h"
#include "ovygram/plugin_install_view.h"
#include "ovygram/plugin_list_view.h"
#include "ovygram/plugin_management_view.h"
#include "ovygram/plugin_ui_tree.h"
#include "ovygram/plugin_proxy_view.h"
#include "ovygram/telemetry_view.h"
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
#include <QtCore/QThreadPool>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QPainter>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QTextEdit>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

rpl::event_stream<> UpdateRequests;
const auto SampleInstallationId = u"1247457d-0214-4fd2-b8fd-8fac5b72e41a"_q;

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
	~Integration() {
		QThreadPool::globalInstance()->waitForDone();
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
		{ u"name"_q, u"Proxy Tunnel (Windows)"_q },
		{ u"name_i18n"_q, QJsonObject{
			{ u"en"_q, u"Proxy Tunnel (Windows)"_q },
			{ u"ru"_q, u"Прокси-туннель (Windows)"_q },
		} },
		{ u"id"_q, u"proxy_tunnel_windows"_q },
		{ u"plugin_id"_q, u"proxy_tunnel_windows"_q },
		{ u"version"_q, u"1.0.3"_q },
		{ u"display_author"_q, Ovygram::Tr(u"Unknown author", u"Неизвестный автор") },
		{ u"description"_q,
			u"Import proxy profiles from HTTPS subscriptions, text, Base64 and JSON. "
			u"Server search and automatic updates."_q },
		{ u"description_i18n"_q, QJsonObject{
			{ u"en"_q,
				u"Import proxy profiles from HTTPS subscriptions, text, Base64 and JSON. "
				u"Server search and automatic updates."_q },
			{ u"ru"_q,
				u"Импорт профилей прокси из HTTPS-подписок, текста, Base64 и JSON. "
				u"Поиск серверов и автообновление."_q },
		} },
		{ u"release"_q, true },
		{ u"enabled"_q, true },
		{ u"loaded"_q, true },
		{ u"requirements"_q, QJsonArray{ u"pyyaml==6.0.2"_q } },
		{ u"trust"_q, QJsonObject{ { u"status"_q, u"unverified"_q } } },
	};
}

QJsonArray SamplePluginList() {
	auto subscriptions = SamplePlugin();
	subscriptions.insert(u"name"_q, u"VLESS · Subscriptions (Windows)"_q);
	subscriptions.insert(u"name_i18n"_q, QJsonObject{
		{ u"en"_q, u"VLESS · Subscriptions (Windows)"_q },
		{ u"ru"_q, u"VLESS · Подписки (Windows)"_q },
	});
	subscriptions.insert(u"version"_q, u"1.0.8"_q);
	subscriptions.insert(u"installation_id"_q, u"8e88c753"_q);
	return {
		QJsonObject{
			{ u"name"_q, u"Ovygram Palette"_q },
			{ u"name_i18n"_q, QJsonObject{
				{ u"en"_q, u"Ovygram Palette"_q },
				{ u"ru"_q, u"Палитра Ovygram"_q },
			} },
			{ u"installation_id"_q, u"c65e509c"_q },
			{ u"plugin_id"_q, u"palette_lab"_q },
			{ u"version"_q, u"1.1.0"_q },
			{ u"description"_q, u"Adds commands for working with colors."_q },
			{ u"description_i18n"_q, QJsonObject{
				{ u"en"_q, u"Adds commands for working with colors."_q },
				{ u"ru"_q, u"Добавляет команды для работы с цветом."_q },
			} },
			{ u"enabled"_q, true },
			{ u"loaded"_q, true },
			{ u"trust"_q, QJsonObject{ { u"status"_q, u"unverified"_q } } },
		},
		subscriptions,
		QJsonObject{
			{ u"name"_q, u"What is this? (Wikipedia)"_q },
			{ u"name_i18n"_q, QJsonObject{
				{ u"en"_q, u"What is this? (Wikipedia)"_q },
				{ u"ru"_q, u"Что это? (Википедия)"_q },
			} },
			{ u"installation_id"_q, u"1c03ee79"_q },
			{ u"plugin_id"_q, u"wikipedia"_q },
			{ u"version"_q, u"1.0.1"_q },
			{ u"description"_q, u"Looks up selected text on Wikipedia."_q },
			{ u"description_i18n"_q, QJsonObject{
				{ u"en"_q, u"Looks up selected text on Wikipedia."_q },
				{ u"ru"_q, u"Ищет выделенный текст в Википедии."_q },
			} },
			{ u"trust"_q, QJsonObject{ { u"status"_q, u"verified"_q } } },
			{ u"update"_q, QJsonObject{ { u"latest_version"_q, u"1.0.2"_q } } },
		},
	};
}

QJsonArray SampleSettings(bool longForm) {
	auto fields = QJsonArray{ QJsonObject{
		{ u"kind"_q, u"link"_q },
		{ u"key"_q, u"open"_q },
		{ u"action_id"_q, u"sample-settings"_q },
		{ u"text"_q, Ovygram::Tr(u"Open \"Proxy Tunnel\"", u"Открыть «Прокси-туннель»") },
		{ u"description"_q, Ovygram::Tr(
			u"Also available from the side menu above the chat list",
			u"Также доступно из бокового меню над списком чатов") },
	} };
	if (longForm) {
		for (auto index = 1; index <= 18; ++index) {
			fields.append(QJsonObject{
				{ u"kind"_q, u"switch"_q },
				{ u"key"_q, QString::number(index) },
				{ u"text"_q, Ovygram::Tr(u"Setting %1", u"Настройка %1").arg(index) },
				{ u"value"_q, index % 2 == 0 },
			});
		}
	}
	return fields;
}

QJsonObject InstallInfo(const QString &scenario) {
	auto metadata = SamplePlugin();
	if (scenario == u"install-long"_q) {
		const auto base = Ovygram::I18n::PluginDescription(metadata);
		metadata.insert(u"description"_q,
			(base + u"\n\n"_q).repeated(12)
			+ Ovygram::Tr(u"End of full description.", u"Конец полного описания."));
		metadata.remove(u"description_i18n"_q);
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
	SettingsLayer(QWidget *parent, QString name, int desiredWidth, bool hasBack = true)
	: Ui::LayerWidget(parent)
	, _desiredWidth(desiredWidth)
	, _title(Ui::CreateChild<Ui::FlatLabel>(this, std::move(name), st::boxTitle))
	, _back(Ui::CreateChild<Ui::IconButton>(this, st::separatePanelBack))
	, _close(Ui::CreateChild<Ui::IconButton>(this, st::boxTitleClose))
	, _scroll(Ui::CreateChild<Ui::ScrollArea>(this, st::boxScroll))
	, _content(_scroll->setOwnedWidget(object_ptr<Ui::VerticalLayout>(_scroll))) {
		_back->setAccessibleName(Ovygram::Tr(u"Back", u"Назад"));
		_back->setVisible(hasBack);
		_close->setAccessibleName(Ovygram::Tr(u"Close", u"Закрыть"));
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
		_back->moveToLeft(0, (head - _back->height()) / 2, width);
		_close->moveToRight(0, (head - _close->height()) / 2, width);
		_title->resizeToWidth(width - 2 * head);
		_title->moveToLeft(_back->isHidden() ? st::boxTitlePosition.x() : head,
			(head - _title->height()) / 2, width);
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
		} else if (_scenario == u"removal"_q) {
			showRemoval();
			finishSoon();
		} else if (_scenario == u"update-channel"_q || _scenario == u"update-channel-ready"_q) {
			showUpdateChannel(_scenario == u"update-channel-ready"_q);
			finishSoon();
		} else if (_scenario == u"diagnostics"_q) {
			showDiagnostics();
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
	void exitLater(int code) {
		if (_exiting) {
			return;
		}
		_exiting = true;
		QTimer::singleShot(0, qApp, [code] { QCoreApplication::exit(code); });
	}

	void showDetails(QJsonObject plugin, QJsonArray fields) {
		_plugin = std::move(plugin);
		_fields = std::move(fields);
		auto layer = object_ptr<SettingsLayer>(&_layers, Ovygram::I18n::PluginName(_plugin),
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
			auto background = object_ptr<SettingsLayer>(&_layers, Ovygram::Tr(u"Plugins", u"Плагины"),
				st::settingsPluginsLayerWidth + style::ConvertScale(36));
			const auto card = Ovygram::AddPluginCard(background->content());
			Ui::AddSkip(card, style::ConvertScale(12));
			Ovygram::AddPluginHeader(card, Ovygram::Tr(u"Desktop Extensions", u"Расширения рабочего стола"), [] {}, true, false, [] {});
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
		const auto offer = QJsonObject{ {u"label"_q, Ovygram::Tr(u"Proxy Tunnel", u"Прокси-туннель")}, {u"owner"_q, u"8e88c753"_q},
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
			box->setTitle(Ovygram::Tr(u"Proxy Tunnel", u"Прокси-туннель"));
			box->addRow(object_ptr<Ui::FlatLabel>(box,
				Ovygram::Tr(u"Sample subscriptions section", u"Тестовый раздел подписок"), st::boxLabel));
			box->addButton(rpl::single(Ovygram::Tr(u"Close", u"Закрыть")), [=] { box->closeBox(); });
		}), Ui::LayerOption::KeepOther, anim::type::instant);
	}

	void showRemoval() {
		if (!_details && !_list) {
			showDetails(SamplePlugin(), SampleSettings(false));
		}
		_removalChoice = QString();
		_layers.showBox(Box([this](not_null<Ui::GenericBox*> box) {
			Ovygram::BuildPluginRemovalBox(box, rpl::single(Ovygram::Tr(u"Cancel", u"Отмена")), [this](bool keepData) {
				_removalChoice = keepData ? u"keep"_q : u"delete"_q;
			});
		}), Ui::LayerOption::KeepOther, anim::type::instant);
	}

	void showUpdateChannel(bool withReadyRelease) {
		_updateChannelChoice = std::nullopt;
		auto data = Ovygram::UpdateChannelBoxData{
			.currentChannel = withReadyRelease
				? Ovygram::UpdateChannel::Beta
				: Ovygram::UpdateChannel::Stable,
			.currentVersionLabel = u"0.1.0-7.4.6 (build #4)"_q,
		};
		if (withReadyRelease) {
			data.readyRelease = Ovygram::UpdateRelease{
				.build = 5,
				.ovygramVersion = u"0.1.1-beta.1"_q,
				.telegramVersion = u"7.4.6"_q,
				.telegramVersionNum = 7004006,
				.fileName = u"ovygram-0.1.1-beta.1-7.4.6-win64-release.zip"_q,
				.sha256 = QString(64, 'a'),
				.size = 54'800'000,
				.url = u"https://github.com/Ovygram/Ovygram/releases/download/update-win64/pkg.zip"_q,
				.commit = u"a1b2c3d4e5f6"_q,
				.notes = Ovygram::Tr(
					u"• Added three-channel auto-updates (stable / beta / canary) with Ed25519 signature verification.\n"
					u"• Native update banner in the chat list and channel selector in Advanced settings.\n"
					u"• Safe python/ folder replacement during update without losing account or plugin data.",
					u"• Добавлена система автообновлений с тремя каналами (stable / beta / canary) и проверкой подписи Ed25519.\n"
					u"• Нативная плашка обновления в списке чатов и выбор канала в Продвинутых настройках.\n"
					u"• Безопасная замена папки python/ при обновлении без потери данных аккаунта и плагинов."),
				.sourceChannel = Ovygram::UpdateChannel::Beta,
			};
		}
		_layers.showBox(Box([this, data = std::move(data)](not_null<Ui::GenericBox*> box) mutable {
			Ovygram::BuildUpdateChannelBox(box, std::move(data), [this](Ovygram::UpdateChannel chosen) {
				_updateChannelChoice = chosen;
			});
		}), Ui::LayerOption::KeepOther, anim::type::instant);
	}

	void showDiagnostics() {
		auto layer = object_ptr<SettingsLayer>(&_layers,
			Ovygram::Tr(u"Diagnostics", u"Диагностика"), st::settingsPluginsLayerWidth, false);
		Ovygram::Telemetry::AddDiagnosticsRows(layer->content(), SampleInstallationId, _telemetryEnabled,
			[this](bool enabled) { _telemetryEnabled = enabled; }, [this] {
				_layers.showBox(Box([](not_null<Ui::GenericBox*> box) {
					Ovygram::Telemetry::BuildSupportBox(box, Ovygram::Tr(
						u"Installation ID: %1\nOvygram: 0.1.1 (build 4)\nPlatform: linux-x64\nUpdate channel: stable"_q,
						u"ID установки: %1\nOvygram: 0.1.1 (сборка 4)\nПлатформа: linux-x64\nКанал обновлений: stable"_q
					).arg(SampleInstallationId));
				}), Ui::LayerOption::KeepOther, anim::type::instant);
			});
		_layers.showSpecialLayer(std::move(layer), anim::type::instant);
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
	rpl::variable<QString> _listNotice;
	bool _listLaunchBlocked = false;
	QJsonObject _plugin;
	QJsonArray _fields;
	QString _installChoice;
	QString _removalChoice;
	std::optional<Ovygram::UpdateChannel> _updateChannelChoice;
	bool _telemetryEnabled = true;
	QProcess _process;
	std::map<QString, std::unique_ptr<Surface>> _surfaces;
	QPointer<QWidget> _captureWindow;
	bool _updating = false;
	bool _finishQueued = false;
	bool _exiting = false;
};

void Preview::showListNotice(QString title, QString body) {
	_layers.showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(title));
		box->addRow(object_ptr<Ui::FlatLabel>(box, body, st::boxLabel));
		box->addButton(rpl::single(Ovygram::Tr(u"Close", u"Закрыть")), [=] { box->closeBox(); });
	}), Ui::LayerOption::KeepOther, anim::type::instant);
}

void Preview::showPluginList() {
	const auto before = _scenario == u"list-before"_q;
	auto layer = object_ptr<SettingsLayer>(&_layers, Ovygram::Tr(u"Plugins", u"Плагины"), st::settingsPluginsLayerWidth, false);
	_list = layer.data();
	const auto layout = _list->content();
	const auto import = [this] { showInstaller(InstallInfo(u"install-new"_q)); };
	if (before) {
		_listSearch = PreviewPluginList::AddSearch(layout);
		PreviewPluginList::AddActions(layout, import, [this] {
			showListNotice(
				Ovygram::Tr(u"Plugin updates", u"Обновления плагинов"),
				Ovygram::Tr(u"One update is available.", u"Доступно одно обновление."));
		});
	} else {
		const auto check = [this] {
			const auto hasUpdate = std::any_of(_listPlugins.cbegin(), _listPlugins.cend(), [](const QJsonValue &entry) {
				return entry.toObject().value(u"update"_q).isObject();
			});
			_listNotice = hasUpdate
				? Ovygram::Tr(u"New or safe versions found: 1.", u"Найдены новые или безопасные версии: 1.")
				: Ovygram::Tr(u"Check completed. No new verified versions.", u"Проверка завершена. Новых проверенных версий нет.");
			_list->parentResized();
		};
		_listSearch = Ovygram::AddPluginListSearch(layout);
		Ovygram::AddPluginListActions(layout, import, check);
		Ovygram::AddPluginListNotice(layout, _listNotice.value());
		check();
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
		const auto fields = plugin.value(u"plugin_id"_q) == u"proxy_tunnel_windows"_q
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
	const auto log = [this] {
		showListNotice(
			Ovygram::Tr(u"Plugin log", u"Журнал плагинов"),
			Ovygram::Tr(u"The sample profile log is empty.", u"Журнал тестового профиля пуст."));
	};
	if (_scenario == u"list-before"_q) {
		PreviewPluginList::BuildRows(_listRows, _listPlugins, query, open, setEnabled,
			[this] {
				showListNotice(
					Ovygram::Tr(u"Share plugin", u"Отправить плагин"),
					Ovygram::Tr(u"Recipient picker is available in the client.", u"Выбор получателя доступен в клиенте."));
			},
			[this] { showRemoval(); },
			log);
	} else {
		const auto applyUpdate = [this](QString id) {
			QTimer::singleShot(0, this, [=, this] {
				if (!_list) {
					return;
				}
				for (auto i = 0; i != _listPlugins.size(); ++i) {
					auto plugin = _listPlugins.at(i).toObject();
					if (plugin.value(u"installation_id"_q).toString() == id) {
						const auto offer = plugin.take(u"update"_q).toObject();
						plugin.insert(u"version"_q, offer.value(u"latest_version"_q));
						_listPlugins.replace(i, plugin);
						break;
					}
				}
				_listNotice = QString();
				buildPluginList();
			});
		};
		Ovygram::AddPluginListRows(_listRows, _listPlugins, query, _listLaunchBlocked,
			open, setEnabled, applyUpdate, log);
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
				exitLater(1);
				break;
			}
		}
	});
	connect(&_process, &QProcess::errorOccurred, this, [this] {
		if (_exiting) return;
		qCritical().noquote() << _process.errorString();
		exitLater(1);
	});
	connect(&_process, &QProcess::finished, this, [this](int code) {
		if (_exiting) return;
		if (code || (!_details && _surfaces.empty())) {
			qCritical("The plugin host left the preview.");
			exitLater(1);
		}
	});
	_process.start(python, {
		u"-B"_q, QString::fromUtf8(OVYGRAM_PREVIEW_PYTHON),
		QFileInfo(plugin).absoluteFilePath(), menu, setting, dependencies,
		Ovygram::I18n::Language()
	});
	QTimer::singleShot(10000, this, [this] {
		if (!_exiting && !_details && _surfaces.empty()) {
			qCritical("No plugin window opened. Check the menu or settings key.");
			exitLater(1);
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
	} else if (method == u"appearance.palette"_q) {
		write({
			{ u"kind"_q, u"reply"_q },
			{ u"request"_q, QJsonObject{
				{ u"id"_q, command.value(u"id"_q) },
				{ u"result"_q, Ovygram::PluginStylePalette() },
			} },
		});
	} else if (method == u"ui.clipboard"_q) {
		QGuiApplication::clipboard()->setText(command.value(u"text"_q).toString());
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
			box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); })->setAccessibleName(Ovygram::Tr(u"Close", u"Закрыть"));
			if (const auto cap = command.value(u"max_content_width"_q).toInt(); cap > 0) {
				*wrapper = Ovygram::PluginUiTree::CenterContent(box, std::move(*wrapper), 0, cap);
			}
			box->addRow(std::move(*wrapper), style::al_justify);
			if (kind == u"dialog"_q || *footerAction) {
				Ovygram::PluginUiTree::AddDialogButtons(box, std::move(*footerAction));
			}
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
			exitLater(1);
		}
	});
}

void Preview::verify() {
	if (_scenario == u"diagnostics"_q) {
		const auto toggle = Ovygram::Tr(u"Send usage statistics and diagnostics", u"Отправлять статистику и диагностику");
		Click(this, toggle);
		Check(!_telemetryEnabled, "Diagnostics toggle did not disable telemetry.");
		Click(this, toggle);
		Check(_telemetryEnabled, "Diagnostics toggle did not re-enable telemetry.");
		Click(this, Ovygram::Tr(u"Copy installation ID", u"Скопировать ID установки"));
		Check(QGuiApplication::clipboard()->text() == SampleInstallationId, "Installation ID was not copied.");
		Click(this, Ovygram::Tr(u"Copy details for support", u"Скопировать сведения для поддержки"));
		const auto top = const_cast<Ui::LayerWidget*>(_layers.topShownLayer());
		Click(top, Ovygram::Tr(u"Copy", u"Скопировать"));
		_layers.finishAnimating();
		Check(QGuiApplication::clipboard()->text().contains(SampleInstallationId)
			&& QGuiApplication::clipboard()->text().contains(u"linux-x64"_q), "Support details were not copied.");
	} else if (_scenario == u"proxies"_q) {
		const auto resetApi = Ovygram::Tr(u"Reset route: Telegram requests", u"Сбросить маршрут: Запросы Telegram");
		const auto reset = dynamic_cast<Ui::RoundButton*>(ByName(this, resetApi));
		Check(reset && !reset->isDisabled(), "The assigned route cannot be reset.");
		Click(this, resetApi);
		Check(reset->isDisabled(), "Reset did not restore the default route.");
		Click(this, Ovygram::Tr(u"Reset all", u"Сбросить все"));
		const auto defaultReset = dynamic_cast<Ui::RoundButton*>(ByName(this,
			Ovygram::Tr(u"Reset route: Default proxy", u"Сбросить маршрут: Прокси по умолчанию")));
		Check(defaultReset && defaultReset->isDisabled(), "Reset all did not clear the routes.");
	} else if (_scenario == u"removal"_q) {
		const auto top = const_cast<Ui::LayerWidget*>(_layers.topShownLayer());
		const auto cancel = Ovygram::Tr(u"Cancel", u"Отмена");
		const auto remove = Ovygram::Tr(u"Remove", u"Удалить");
		const auto keep = Ovygram::Tr(u"Keep settings and data", u"Сохранить настройки и данные");
		Check(top && ByName(top, cancel) && ByName(top, remove) && ByName(top, keep),
			"Removal box controls are missing.");
		Click(top, keep);
		Click(top, remove);
		_layers.finishAnimating();
		Check(_removalChoice == u"keep"_q, "Removal checkbox did not preserve plugin data.");
		showRemoval();
		const auto second = const_cast<Ui::LayerWidget*>(_layers.topShownLayer());
		Click(second, remove);
		_layers.finishAnimating();
		Check(_removalChoice == u"delete"_q, "Removal default action did not delete plugin data.");
	} else if (_scenario == u"update-channel"_q || _scenario == u"update-channel-ready"_q) {
		const auto top = const_cast<Ui::LayerWidget*>(_layers.topShownLayer());
		const auto stable = Ovygram::UpdateChannelTitle(Ovygram::UpdateChannel::Stable);
		const auto beta = Ovygram::UpdateChannelTitle(Ovygram::UpdateChannel::Beta);
		const auto canary = Ovygram::UpdateChannelTitle(Ovygram::UpdateChannel::Canary);
		const auto save = Ovygram::Tr(u"Save", u"Сохранить");
		const auto cancel = Ovygram::Tr(u"Cancel", u"Отмена");
		Check(top && ByName(top, stable) && ByName(top, beta)
			&& ByName(top, canary) && ByName(top, save) && ByName(top, cancel),
			"Update channel controls are missing.");
		Click(top, canary);
		Click(top, save);
		_layers.finishAnimating();
		Check(_updateChannelChoice == Ovygram::UpdateChannel::Canary,
			"Update channel box did not save selected Canary channel.");
		showUpdateChannel(_scenario == u"update-channel-ready"_q);
	} else if (_scenario == u"list-after"_q) {
		const auto first = Ovygram::Tr(u"VLESS · Subscriptions (Windows)", u"VLESS · Подписки (Windows)");
		const auto wiki = Ovygram::Tr(u"What is this? (Wikipedia)", u"Что это? (Википедия)");
		const auto updatePrefix = Ovygram::Tr(u"Update · ", u"Обновить · ");
		const auto update = updatePrefix + wiki;
		const auto enableWiki = Ovygram::Tr(u"Enable %1", u"Включить %1").arg(wiki);
		const auto disableWiki = Ovygram::Tr(u"Disable %1", u"Отключить %1").arg(wiki);
		const auto button = ByName(_list, update);
		Check(ByName(_list, first), "A plugin is missing from the list.");
		Check(ByName(_list, Ovygram::Tr(u"Unverified", u"Не проверен"))
			&& ByName(_list, Ovygram::Tr(u"Verified", u"Проверен")),
			"A plugin trust badge is missing.");
		Check(ByName(_list, Ovygram::Tr(u"New version 1.0.2", u"Новая версия 1.0.2")) && button
			&& !ByName(_list, updatePrefix + first), "Update was offered for the wrong plugins.");
		const auto row = button->parentWidget();
		Check(row->childAt(button->mapToParent(button->rect().center())) == button,
			"The update button is covered by the row controls.");
		_listLaunchBlocked = true;
		buildPluginList();
		const auto start = dynamic_cast<Ui::SettingsButton*>(ByName(_list, enableWiki));
		const auto stop = dynamic_cast<Ui::SettingsButton*>(ByName(_list,
			Ovygram::Tr(u"Disable %1", u"Отключить %1").arg(first)));
		Check(start && start->isDisabled() && stop && !stop->isDisabled(),
			"Runtime recovery must prevent starting plugins and allow stopping them.");
		_listLaunchBlocked = false;
		buildPluginList();
		_listSearch->setFocus();
		const auto edit = _listSearch->rawTextEdit().get();
		QTest::keyClicks(edit, "wikipedia");
		Check(!ByName(_list, first) && ByName(_list, wiki), "Plugin ID search did not filter the list.");
		QTest::keyClick(edit, Qt::Key_A, Qt::ControlModifier);
		QTest::keyClicks(edit, "missing");
		Check(ByName(_list, Ovygram::Tr(u"No plugins found.", u"Плагины не найдены.")), "Empty search result is missing.");
		Click(_list, Ovygram::Tr(u"Clear search", u"Очистить поиск"));
		Check(ByName(_list, first), "Clearing search did not restore the list.");
		Click(_list, enableWiki);
		Check(ByName(_list, disableWiki), "The plugin did not start.");
		Click(_list, disableWiki);
		Check(ByName(_list, enableWiki), "The plugin did not stop.");
		Click(_list, update);
		Check(!_details && ByName(_list, Ovygram::Tr(u"Stopped · v1.0.2", u"Остановлен · v1.0.2"))
			&& !ByName(_list, update) && ByName(_list, enableWiki),
			"Update did not finish in the list or changed the toggle.");
		Click(_list, Ovygram::Tr(u"Check for updates", u"Проверить обновления"));
		Check(ByName(_list, Ovygram::Tr(
			u"Check completed. No new verified versions.",
			u"Проверка завершена. Новых проверенных версий нет.")),
			"Update check feedback is missing from the list.");
		Click(_list, first);
		Check(_details && ByName(_details, Ovygram::Tr(u"Remove plugin", u"Удалить плагин")),
			"Plugin details did not open.");
		Click(_details, Ovygram::Tr(u"Back", u"Назад"));
		Check(_list && ByName(_list, first), "Back did not return to the plugin list.");
	} else if (_scenario == u"list-before"_q) {
		Check(ByName(_list, Ovygram::Tr(u"Share", u"Отправить"))
			&& ByName(_list, Ovygram::Tr(u"Remove", u"Удалить")),
			"The original card actions are missing.");
	} else if (_scenario.startsWith(u"install-"_q)) {
		const auto first = _scenario == u"install-new"_q || _scenario == u"install-long"_q;
		Check(bool(ByName(this, Ovygram::Tr(u"New version", u"Новая версия"))) == !first, "Unexpected version heading.");
		Check(ByName(this, Ovygram::Tr(u"Close", u"Закрыть")), "Installer has no close control.");
		Check(bool(Scrolls(this)) == (_scenario == u"install-long"_q), "Unexpected installer scrolling.");
		if (!first) {
			Check(ByName(this, Ovygram::Tr(u"Install separately", u"Установить отдельно")), "Secondary install action is missing.");
			Click(this, Ovygram::Tr(u"Update", u"Обновить"));
			Check(_installChoice == u"8e88c753"_q, "Update chose the wrong instance.");
		} else {
			Click(this, Ovygram::Tr(u"Install", u"Установить"));
			Check(_installChoice == u"install"_q, "Fresh install chose the wrong action.");
		}
		if (_action.isEmpty()) {
			const auto fileDetails = Ovygram::Tr(u"File details", u"Подробности файла");
			Click(this, fileDetails);
			Check(ByName(this, u"ID: proxy_tunnel_windows"_q), "File details didn't expand.");
			Click(this, fileDetails);
		}
	} else {
		const auto enabled = _scenario != u"stopped"_q;
		Check(_details && (_details->scrollMaximum() > 0) == (_scenario == u"long"_q), "Unexpected settings scrolling.");
		if (enabled) {
			Click(this, Ovygram::Tr(u"Disable plugin", u"Отключить плагин"));
			Check(ByName(this, Ovygram::Tr(u"Stopped", u"Остановлен")), "The stop status didn't change.");
			Click(this, Ovygram::Tr(u"Enable plugin", u"Включить плагин"));
			Check(ByName(this, Ovygram::Tr(u"Running", u"Запущен")), "The start status didn't change.");
			Click(this, Ovygram::Tr(u"Open \"Proxy Tunnel\"", u"Открыть «Прокси-туннель»"));
			Check(ByName(this, Ovygram::Tr(u"Sample subscriptions section", u"Тестовый раздел подписок")),
				"Settings destination did not open.");
			const auto top = const_cast<Ui::LayerWidget*>(_layers.topShownLayer());
			Click(top, Ovygram::Tr(u"Close", u"Закрыть"));
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
			exitLater(1);
			return;
		}
		qInfo().noquote() << _output << pixmap.width() << "×" << pixmap.height();
	}
	// A separate SDK panel handles Close itself, so it can veto QApplication's quit event.
	exitLater(0);
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
		{ u"scenario"_q, u"list-before, list-after, details, stopped, long, install-new, install-update, install-multiple, install-long, proxies, removal, update-channel, update-channel-ready, diagnostics."_q,
			u"name"_q, u"details"_q },
		{ u"lang"_q, u"Ovygram UI language code (e.g. en, ru, de)."_q, u"code"_q, u"en"_q },
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
		{ u"python"_q, u"System CPython for the SDK host."_q, u"executable"_q, u"python3"_q },
	});
	parser.process(app);
	try {
		const auto scenario = parser.value(u"scenario"_q);
		const auto plugin = parser.value(u"plugin-file"_q);
		Ovygram::I18n::SetLanguage(parser.value(u"lang"_q));
		Check(QStringList{ u"list-before"_q, u"list-after"_q, u"details"_q, u"stopped"_q, u"long"_q, u"install-new"_q, u"install-update"_q,
			u"install-multiple"_q, u"install-long"_q, u"proxies"_q, u"removal"_q, u"update-channel"_q, u"update-channel-ready"_q,
			u"diagnostics"_q }.contains(scenario), "Unknown preview scenario.");
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
		style::internal::StartFonts();
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
