#include "ovygram/plugin_install.h"
#include "ovygram/plugin_media.h"

#include "core/application.h"
#include "core/plugin_runtime.h"
#include "ovygram/plugin_trust_badge.h"
#include "ovygram/protected_ui.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "window/window_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

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

bool Trusted(const QString &status) {
	return status == u"verified"_q || status == u"official"_q;
}

QString StatusText(const QString &status) {
	if (status == u"official"_q) return u"Официальный плагин"_q;
	if (status == u"verified"_q) return u"Проверенный плагин"_q;
	if (status == u"blocked"_q) return u"Опасный плагин — установка запрещена"_q;
	if (status == u"revoked"_q) return u"Проверка отозвана"_q;
	if (status == u"verification_unavailable"_q) return u"Проверка недоступна"_q;
	return u"Не проверен"_q;
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
		box->setTitle(rpl::single(u"Установка плагина"_q));
		box->setWidth(st::settingsPluginsLayerWidth);
		box->setMaxHeight(680);
		_rows = box->addRow(object_ptr<Ui::VerticalLayout>(box), style::margins(), style::al_justify);
		cancel();
		box->setFocusCallback([this] {
			if (_cancel) _cancel->setFocus();
		});
		label(_rows, u"Чтение метаданных и проверка файла…"_q);
		_timer = new QTimer(this);
		connect(_timer, &QTimer::timeout, this, [this] { poll(); });
		try {
			_token = Request({ { u"action"_q, u"prepare_install"_q }, { u"path"_q, path } }).toString();
			_timer->start(200);
		} catch (const std::exception &error) {
			label(_rows, QString::fromUtf8(error.what()));
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
	void label(
			not_null<Ui::VerticalLayout*> rows,
			QString text,
			const style::FlatLabel &style = st::settingsPluginBody) {
		const auto value = rows->add(
			object_ptr<Ui::FlatLabel>(rows, std::move(text), style),
			st::settingsPluginTextMargin,
			style::al_justify);
		// FlatLabel's QString overload is multiline plain text, with no
		// rich-text entities, parsed links, elision or height cap.
		value->setBreakEverywhere(true);
		value->setSelectable(true);
	}

	void cancel() {
		_cancel = _box->addButton(rpl::single(u"Отмена"_q), [this] { _box->closeBox(); });
	}

	void action(QString text, Fn<void()> callback, Ui::VerticalLayout *choices = nullptr) {
		auto invoke = [callback = std::move(callback)] {
			// The callback may rebuild the controls containing this button.
			const auto invoke = callback;
			invoke();
		};
		if (choices) {
			const auto button = choices->add(object_ptr<Ui::RoundButton>(
				choices, rpl::single(std::move(text)), st::defaultLightButton),
				st::boxRowPadding, style::al_right);
			button->setClickedCallback(std::move(invoke));
		} else {
			_box->clearButtons();
			_box->addButton(rpl::single(std::move(text)), std::move(invoke), st::defaultActiveButton);
			cancel();
			if (_cancel) _cancel->setFocus();
		}
	}

	void poll() {
		try {
			const auto info = Request({ { u"action"_q, u"prepared_install"_q }, { u"token"_q, _token } }).toObject();
			const auto state = info.value(u"state"_q).toString();
			if (state == u"pending"_q) return;
			_timer->stop();
			Clear(_rows);
			if (state != u"ready"_q) {
				label(_rows, info.value(u"error"_q).toString());
			} else {
				_info = info;
				showChoices();
			}
		} catch (const std::exception &error) {
			_timer->stop();
			label(_rows, QString::fromUtf8(error.what()));
		}
		_rows->resizeToWidth(_rows->widthNoMargins());
	}

	not_null<Ui::VerticalLayout*> details(const QJsonObject &metadata, const QJsonObject &trust) {
		const auto card = _rows->add(
			object_ptr<Ui::VerticalLayout>(_rows), st::settingsPluginCardMargin, style::al_justify);
		card->paintRequest() | rpl::on_next([=] {
			auto p = QPainter(card);
			const auto quality = PainterHighQualityEnabler(p);
			p.setPen(Qt::NoPen);
			p.setBrush(st::windowBgOver);
			p.drawRoundedRect(card->rect(), st::settingsPluginCardRadius, st::settingsPluginCardRadius);
		}, card->lifetime());
		Ui::AddSkip(card, 12);
		label(card, metadata.value(u"name"_q).toString(), st::settingsPluginInstallTitle);
		label(card, u"Версия %1 · %2"_q.arg(metadata.value(u"version"_q).toString(),
			metadata.value(u"id"_q).toString(metadata.value(u"plugin_id"_q).toString())),
			st::settingsPluginCardMeta);
		label(card, u"Автор: "_q + metadata.value(u"display_author"_q).toString(
			metadata.value(u"author"_q).toString()), st::settingsPluginCardMeta);
		label(card, metadata.value(u"description"_q).toString());
		const auto status = trust.value(u"status"_q).toString();
		AddPluginTrustBadge(card, StatusText(status), status);
		if (const auto reason = trust.value(u"reason"_q).toString(); !reason.isEmpty()) {
			label(card, reason, st::settingsPluginCardMeta);
		} else if (status == u"unverified"_q) {
			label(card, u"Файл ещё не проходил модерацию."_q, st::settingsPluginCardMeta);
		}
		Ui::AddSkip(card, 4);
		return card;
	}

	void showChoices() {
		const auto metadata = _info.value(u"metadata"_q).toObject();
		const auto trust = _info.value(u"trust"_q).toObject();
		const auto status = trust.value(u"status"_q).toString();
		label(_rows, u"Новая версия"_q, st::settingsPluginCardMeta);
		const auto selected = details(metadata, trust);
		label(selected, metadata.value(u"release"_q).toBool()
			? u"Публичная версия — доступна для механизма обновлений"_q
			: u"Непубличный плагин"_q, st::settingsPluginCardMeta);

		const auto disclosure = selected->add(object_ptr<Ui::SettingsButton>(
			selected, rpl::single(u"Подробности файла"_q), st::settingsPluginCardAction));
		const auto technical = selected->add(object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			selected, object_ptr<Ui::VerticalLayout>(selected)), style::al_justify);
		label(technical->entity(), u"ID: "_q + metadata.value(u"id"_q).toString(),
			st::settingsPluginCardMeta);
		label(technical->entity(), u"SHA-256: "_q + _info.value(u"sha256"_q).toString(),
			st::settingsPluginCardMeta);
		technical->toggleOn(_showDetails.value());
		technical->finishAnimating();
		disclosure->setClickedCallback([this] { _showDetails = !_showDetails.current(); });
		if (status == u"blocked"_q) return;

		const auto installations = _info.value(u"installations"_q).toArray();
		auto hasTrusted = false;
		auto updateChoices = 0;
		for (const auto &value : installations) {
			const auto old = value.toObject();
			hasTrusted = hasTrusted || Trusted(old.value(u"trust"_q).toObject().value(u"status"_q).toString());
			if (_installationId.isEmpty() || _installationId == old.value(u"installation_id"_q).toString()) {
				++updateChoices;
			}
		}
		const auto separateChoice = _installationId.isEmpty() && (!hasTrusted || !Trusted(status));
		for (const auto &value : installations) {
			const auto old = value.toObject();
			const auto id = old.value(u"installation_id"_q).toString();
			label(_rows, u"Установленный экземпляр "_q + id.left(8), st::settingsPluginCardMeta);
			const auto installed = details(old, old.value(u"trust"_q).toObject());
			if (_installationId.isEmpty() || _installationId == id) {
				action(u"Обновить "_q + id.left(8), [=, this] { confirm(id, false, old); },
					(updateChoices == 1 && !separateChoice) ? nullptr : installed.get());
			}
		}
		if (!_installationId.isEmpty() && installations.isEmpty()) {
			label(_rows, u"ID файла не совпадает с ID обновляемого экземпляра."_q);
			return;
		}
		if (separateChoice) {
			action(installations.isEmpty() ? u"Установить"_q : u"Установить отдельно"_q,
				[=, this] { confirm(QString(), !installations.isEmpty(), QJsonObject()); });
		}
	}

	void confirm(QString id, bool separate, QJsonObject old) {
		const auto trust = _info.value(u"trust"_q).toObject();
		const auto status = trust.value(u"status"_q).toString();
		const auto loss = (old.value(u"previously_trusted"_q).toBool()
			|| Trusted(old.value(u"trust"_q).toObject().value(u"status"_q).toString())) && !Trusted(status);
		const auto unavailable = status == u"verification_unavailable"_q;
		if ((loss || unavailable) && !_warned) {
			Clear(_rows);
			_box->clearButtons();
			cancel();
			details(_info.value(u"metadata"_q).toObject(), trust);
			if (loss) label(_rows, u"Вы заменяете проверенный плагин непроверенной версией. Автор и безопасность "
				"нового файла не подтверждены. Плагин потеряет статус проверки. Можно отменить "
				"замену и установить файл отдельно."_q);
			if (unavailable) label(_rows, u"Проверка недоступна. Невозможно уточнить, заблокирован ли этот файл. "
				"Продолжайте только если доверяете источнику."_q);
			_rows->resizeToWidth(_rows->widthNoMargins());
			_warned = true;
			action(u"Подтверждаю установку"_q, [=, this] { confirm(id, separate, old); });
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
			label(_rows, QString::fromUtf8(error.what()));
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
