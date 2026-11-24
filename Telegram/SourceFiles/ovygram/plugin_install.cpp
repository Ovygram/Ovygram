#include "ovygram/plugin_install.h"
#include "ovygram/plugin_media.h"

#include "core/application.h"
#include "core/plugin_runtime.h"
#include "ovygram/plugin_install_view.h"
#include "ovygram/protected_ui.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/widgets/buttons.h"
#include "window/window_controller.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <rpl/variable.h>

#include <stdexcept>

namespace Ovygram {
namespace {

int ActiveInstallers = 0;

QJsonValue Request(const QJsonObject &request) {
	const auto runtime = Core::App().pluginRuntime();
	if (!runtime) {
		throw std::runtime_error("Встроенный Python недоступен.");
	}
	const auto response = QJsonDocument::fromJson(QByteArray::fromStdString(
		runtime->dispatch(QJsonDocument(request).toJson(QJsonDocument::Compact).toStdString()))).object();
	if (!response.contains(u"result"_q)) {
		throw std::runtime_error(response.value(u"error"_q).toString().toStdString());
	}
	return response.value(u"result"_q);
}

void Clear(not_null<Ui::VerticalLayout*> layout) {
	while (layout->count()) delete layout->widgetAt(0);
}

// State is owned by Telegram's box. There is no separate Qt/system dialog,
// plugin-owned content or implicit acceptance on Enter. The footer remains
// visible while the complete description scrolls in GenericBox's viewport.
class InstallController final : public QObject {
public:
	InstallController(
		not_null<Ui::GenericBox*> box,
		QString path,
		QString installationId,
		Fn<void()> done)
	: QObject(box.get())
	, _box(box.get())
	, _installationId(std::move(installationId))
	, _done(std::move(done)) {
		++ActiveInstallers;
		ProtectSurface(box);
		_rows = PreparePluginInstallBox(box);
		cancel();
		box->setFocusCallback([this] {
			if (_cancel) _cancel->setFocus();
		});
		AddPluginInstallLabel(_rows, u"Чтение метаданных и проверка файла…"_q);
		_timer = new QTimer(this);
		connect(_timer, &QTimer::timeout, this, [this] { poll(); });
		try {
			_token = Request({ { u"action"_q, u"prepare_install"_q }, { u"path"_q, path } }).toString();
			_timer->start(200);
		} catch (const std::exception &error) {
			AddPluginInstallLabel(_rows, QString::fromUtf8(error.what()));
		}
	}

	~InstallController() {
		if (!_token.isEmpty()) {
			try {
				(void)Request({ { u"action"_q, u"discard_install"_q }, { u"token"_q, _token } });
			} catch (...) {
			}
		}
		--ActiveInstallers;
		if (_done) _done();
	}

private:
	void cancel() {
		_cancel = SetPluginInstallAction(_box.data(), QString(), nullptr);
	}

	void poll() {
		try {
			const auto info = Request({ { u"action"_q, u"prepared_install"_q }, { u"token"_q, _token } }).toObject();
			const auto state = info.value(u"state"_q).toString();
			if (state == u"pending"_q) return;
			_timer->stop();
			Clear(_rows);
			if (state != u"ready"_q) {
				AddPluginInstallLabel(_rows, info.value(u"error"_q).toString());
			} else {
				_info = info;
				AddPluginInstallChoices(_box.data(), _rows, _info, _installationId, _showDetails,
					[this](QString id, bool separate, QJsonObject old) {
						confirm(std::move(id), separate, std::move(old));
					},
					[this](Ui::RoundButton *cancel) {
						_cancel = cancel;
						_cancel->setFocus();
					});
			}
		} catch (const std::exception &error) {
			_timer->stop();
			AddPluginInstallLabel(_rows, QString::fromUtf8(error.what()));
		}
		_rows->resizeToWidth(_rows->widthNoMargins());
	}

	void confirm(QString id, bool separate, QJsonObject old) {
		const auto trust = _info.value(u"trust"_q).toObject();
		const auto status = trust.value(u"status"_q).toString();
		const auto loss = (old.value(u"previously_trusted"_q).toBool()
			|| IsTrustedPlugin(old.value(u"trust"_q).toObject().value(u"status"_q).toString())) && !IsTrustedPlugin(status);
		const auto unavailable = status == u"verification_unavailable"_q;
		if ((loss || unavailable) && !_warned) {
			Clear(_rows);
			AddPluginInstallCard(_rows, _info.value(u"metadata"_q).toObject(), trust);
			if (loss) AddPluginInstallLabel(_rows, u"Вы заменяете проверенный плагин непроверенной версией. Автор и безопасность "
				"нового файла не подтверждены. Плагин потеряет статус проверки. Можно отменить "
				"замену и установить файл отдельно."_q);
			if (unavailable) AddPluginInstallLabel(_rows, u"Проверка недоступна. Невозможно уточнить, заблокирован ли этот файл. "
				"Продолжайте только если доверяете источнику."_q);
			_rows->resizeToWidth(_rows->widthNoMargins());
			_warned = true;
			_cancel = SetPluginInstallAction(_box.data(), u"Подтверждаю установку"_q,
				[=, this] { confirm(id, separate, old); });
			if (_cancel) _cancel->setFocus();
			return;
		}
		try {
			(void)Request({ { u"action"_q, u"commit_install"_q }, { u"token"_q, _token },
				{ u"installation_id"_q, id }, { u"separate"_q, separate },
				{ u"allow_unavailable"_q, unavailable && _warned }, { u"allow_loss"_q, loss && _warned } });
			_token.clear();
			_box->closeBox();
		} catch (const std::exception &error) {
			AddPluginInstallLabel(_rows, QString::fromUtf8(error.what()));
			_rows->resizeToWidth(_rows->widthNoMargins());
		}
	}

	QPointer<Ui::GenericBox> _box;
	QPointer<Ui::RoundButton> _cancel;
	QString _token;
	QString _installationId;
	Fn<void()> _done;
	QJsonObject _info;
	Ui::VerticalLayout *_rows = nullptr;
	QTimer *_timer = nullptr;
	rpl::variable<bool> _showDetails = false;
	bool _warned = false;
};

} // namespace

bool InstallationUiActive() {
	return ActiveInstallers != 0 || PluginFilePickerActive();
}

void ShowPluginInstall(const QString &path, const QString &installationId, Fn<void()> done) {
	if (InstallationUiActive()) return;
	const auto window = Core::App().activePrimaryWindow();
	if (!window) {
		if (done) done();
		return;
	}
	window->uiShow()->showBox(Box([=, done = std::move(done)](not_null<Ui::GenericBox*> box) mutable {
		new InstallController(box, path, installationId, std::move(done));
	}));
}

} // namespace Ovygram
