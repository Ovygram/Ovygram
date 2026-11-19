#include "ovygram/vless_proxy.h"

#include "boxes/abstract_box.h"
#include "core/application.h"
#include "ovygram/protected_ui.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QUuid>
#include <QtNetwork/QTcpServer>

#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace Ovygram {
namespace {

constexpr auto kMaxLinkLength = 8192;
constexpr auto kProbeInterval = 100;
constexpr auto kStartupTimeout = 5000;

[[nodiscard]] QString XrayProgram() {
#ifdef Q_OS_WIN
	const auto path = QCoreApplication::applicationDirPath()
		+ u"/vless/xray.exe"_q;
	return QFileInfo(path).isFile() ? path : QString();
#else
	return QStandardPaths::findExecutable(u"xray"_q);
#endif
}

[[nodiscard]] QJsonObject Outbound(const QUrl &url) {
	const auto query = QUrlQuery(url);
	const auto value = [&](const QString &key, QString fallback = {}) {
		const auto result = query.queryItemValue(key, QUrl::FullyDecoded);
		return result.isEmpty() ? fallback : result;
	};
	const auto host = url.host();
	const auto port = url.port();
	const auto id = QUuid(url.userName());
	if (url.scheme() != u"vless"_q
		|| !url.isValid()
		|| host.isEmpty()
		|| port <= 0
		|| id.isNull()
		|| !url.password().isEmpty()
		|| (!url.path().isEmpty() && url.path() != u"/"_q)) {
		return {};
	}
	const auto network = value(u"type"_q, u"tcp"_q);
	const auto security = value(u"security"_q, u"none"_q);
	const auto sni = value(u"sni"_q, host);
	const auto header = value(u"headerType"_q, u"none"_q);
	const auto insecure = value(u"allowInsecure"_q, u"0"_q);
	if (header != u"none"_q
		|| (insecure != u"0"_q && insecure != u"false"_q)
		|| query.hasQueryItem(u"extra"_q)) {
		return {};
	}
	auto stream = QJsonObject{
		{ u"network"_q, network },
		{ u"security"_q, security },
	};
	if (security == u"tls"_q) {
		auto tls = QJsonObject{ { u"serverName"_q, sni } };
		if (const auto fingerprint = value(u"fp"_q); !fingerprint.isEmpty()) {
			tls.insert(u"fingerprint"_q, fingerprint);
		}
		if (const auto alpn = value(u"alpn"_q); !alpn.isEmpty()) {
			tls.insert(u"alpn"_q, QJsonArray::fromStringList(alpn.split(',')));
		}
		stream.insert(u"tlsSettings"_q, tls);
	} else if (security == u"reality"_q) {
		const auto key = value(u"pbk"_q);
		if (key.isEmpty()) {
			return {};
		}
		stream.insert(u"realitySettings"_q, QJsonObject{
			{ u"serverName"_q, sni },
			{ u"fingerprint"_q, value(u"fp"_q, u"chrome"_q) },
			{ u"password"_q, key },
			{ u"shortId"_q, value(u"sid"_q) },
			{ u"spiderX"_q, value(u"spx"_q, u"/"_q) },
		});
	} else if (security != u"none"_q) {
		return {};
	}

	const auto path = value(u"path"_q, u"/"_q);
	const auto transportHost = value(u"host"_q);
	if (network == u"ws"_q) {
		stream.insert(u"wsSettings"_q, QJsonObject{
			{ u"path"_q, path },
			{ u"host"_q, transportHost },
		});
	} else if (network == u"grpc"_q) {
		stream.insert(u"grpcSettings"_q, QJsonObject{
			{ u"serviceName"_q, value(u"serviceName"_q) },
			{ u"authority"_q, value(u"authority"_q) },
			{ u"multiMode"_q, value(u"mode"_q) == u"multi"_q },
		});
	} else if (network == u"httpupgrade"_q) {
		stream.insert(u"httpupgradeSettings"_q, QJsonObject{
			{ u"path"_q, path },
			{ u"host"_q, transportHost },
		});
	} else if (network == u"xhttp"_q || network == u"splithttp"_q) {
		stream.insert(u"xhttpSettings"_q, QJsonObject{
			{ u"path"_q, path },
			{ u"host"_q, transportHost },
			{ u"mode"_q, value(u"mode"_q, u"auto"_q) },
		});
	} else if (network != u"tcp"_q && network != u"raw"_q) {
		return {};
	}

	return {
		{ u"protocol"_q, u"vless"_q },
		{ u"settings"_q, QJsonObject{
			{ u"address"_q, host },
			{ u"port"_q, port },
			{ u"id"_q, id.toString(QUuid::WithoutBraces) },
			{ u"encryption"_q, value(u"encryption"_q, u"none"_q) },
			{ u"flow"_q, value(u"flow"_q) },
		} },
		{ u"streamSettings"_q, stream },
	};
}

[[nodiscard]] QByteArray Config(
		const MTP::ProxyData &proxy,
		const QJsonObject &outbound) {
	const auto inbound = QJsonObject{
		{ u"protocol"_q, u"socks"_q },
		{ u"listen"_q, proxy.host },
		{ u"port"_q, int(proxy.port) },
		{ u"settings"_q, QJsonObject{
			{ u"auth"_q, u"password"_q },
			{ u"accounts"_q, QJsonArray{ QJsonObject{
				{ u"user"_q, proxy.user },
				{ u"pass"_q, proxy.password },
			} } },
		} },
	};
	return QJsonDocument(QJsonObject{
		{ u"log"_q, QJsonObject{ { u"loglevel"_q, u"none"_q } } },
		{ u"inbounds"_q, QJsonArray{ inbound } },
		{ u"outbounds"_q, QJsonArray{ outbound } },
	}).toJson(QJsonDocument::Compact);
}

} // namespace

VlessProxy::VlessProxy() {
	_probe.setProxy(QNetworkProxy::NoProxy);
	_poll.setInterval(kProbeInterval);
	connect(&_poll, &QTimer::timeout, this, [this] { probe(); });
	connect(&_probe, &QTcpSocket::connected, this, [this] {
		_authenticating = false;
		_probe.write("\x05\x01\x02", 3);
	});
	connect(&_probe, &QTcpSocket::readyRead, this, [this] { probeReply(); });
}

VlessProxy::~VlessProxy() {
	stop();
}

QString VlessProxy::start(const QString &link) {
	if (link.size() > kMaxLinkLength) {
		return u"Ссылка VLESS слишком длинная."_q;
	}
	const auto outbound = Outbound(QUrl(link.trimmed(), QUrl::StrictMode));
	if (outbound.isEmpty()) {
		return u"Проверьте ссылку VLESS. Возможно, она содержит неподдерживаемые параметры."_q;
	}
	const auto program = XrayProgram();
	if (program.isEmpty()) {
		return u"Ядро Xray не найдено. В Windows оно входит в комплект Ovygram."_q;
	}

	auto local = QTcpServer();
	local.setProxy(QNetworkProxy::NoProxy);
	if (!local.listen(QHostAddress::LocalHost)) {
		return u"Не удалось выделить локальный порт для VLESS."_q;
	}
	stop();
	_proxy.type = MTP::ProxyData::Type::Socks5;
	_proxy.host = u"127.0.0.1"_q;
	_proxy.port = local.serverPort();
	_proxy.user = u"ovygram"_q;
	_proxy.password = QUuid::createUuid().toString(QUuid::WithoutBraces);
	local.close();

	_process = new QProcess(this);
	_process->setStandardOutputFile(QProcess::nullDevice());
	_process->setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
	_process->setCreateProcessArgumentsModifier(
		[](QProcess::CreateProcessArguments *arguments) {
			arguments->flags |= CREATE_NO_WINDOW;
		});
#endif
	connect(_process, &QProcess::started, this, [
		this, config = Config(_proxy, outbound)
	]() mutable {
		_process->write(std::exchange(config, QByteArray()));
		_process->closeWriteChannel();
	});
	connect(_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart) {
			fail(u"Не удалось запустить Xray."_q);
		}
	});
	connect(_process, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
		fail(_poll.isActive()
			? u"Xray не запустился с настройками из этой ссылки."_q
			: u"Xray остановился. Переподключите или отключите VLESS."_q);
	});
	_status = u"Запуск VLESS…"_q;
	_started.start();
	_poll.start();
	_process->start(program, {
		u"run"_q, u"-format"_q, u"json"_q, u"-config"_q, u"stdin:"_q,
	});
	return {};
}

void VlessProxy::stop() {
	_poll.stop();
	_probe.abort();
	if (const auto process = std::exchange(_process, nullptr)) {
		process->disconnect(this);
		if (process->state() != QProcess::NotRunning) {
			process->kill();
		}
		process->deleteLater();
	}
	_status = u"VLESS отключён."_q;
}

rpl::producer<QString> VlessProxy::status() const {
	return _status.value();
}

void VlessProxy::probe() {
	if (_started.elapsed() >= kStartupTimeout) {
		fail(u"Локальный прокси Xray не ответил."_q);
	} else if (_process->state() == QProcess::Running
		&& _probe.state() == QAbstractSocket::UnconnectedState) {
		_probe.connectToHost(QHostAddress::LocalHost, quint16(_proxy.port));
	}
}

void VlessProxy::probeReply() {
	if (!_poll.isActive() || _probe.bytesAvailable() < 2) {
		return;
	}
	const auto reply = _probe.read(2);
	if (_authenticating) {
		if (reply != QByteArray("\x01\x00", 2)) {
			_probe.abort();
			return;
		}
		_poll.stop();
		_probe.abort();
		Core::App().setVlessProxy(_proxy);
		_status = u"VLESS включён. Статус сети показывает подключение к Telegram."_q;
	} else if (reply == QByteArray("\x05\x02", 2)) {
		const auto user = _proxy.user.toLatin1();
		const auto password = _proxy.password.toLatin1();
		auto auth = QByteArray(1, '\x01');
		auth.append(char(user.size())).append(user);
		auth.append(char(password.size())).append(password);
		_authenticating = true;
		_probe.write(auth);
	} else {
		_probe.abort();
	}
}

void VlessProxy::fail(QString message) {
	stop();
	_status = std::move(message);
}

bool IsVlessLink(const QString &link) {
	return link.startsWith(u"vless://"_q, Qt::CaseInsensitive);
}

void ShowVlessBox(const QString &link) {
	Ui::show(Box([=](not_null<Ui::GenericBox*> box) {
		ProtectSurface(box);
		box->setTitle(rpl::single(u"Подключение VLESS"_q));
		box->setWidth(st::boxWideWidth);
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			Ui::InputField::Mode::SingleLine,
			rpl::single(u"Ссылка vless://"_q),
			link.left(kMaxLinkLength + 1)));
		field->setMaxLength(kMaxLinkLength + 1);

		auto &proxy = Core::App().vlessProxy();
		const auto notice = box->lifetime().make_state<rpl::variable<QString>>();
		proxy.status() | rpl::on_next([=](QString text) {
			*notice = std::move(text);
		}, box->lifetime());
		box->addRow(object_ptr<Ui::FlatLabel>(
			box, notice->value(), st::defaultFlatLabel));
		const auto start = [=, &proxy] {
			if (auto error = proxy.start(field->getLastText()); !error.isEmpty()) {
				field->showError();
				*notice = std::move(error);
			}
		};
		field->submits() | rpl::on_next(start, field->lifetime());
		box->addButton(rpl::single(u"Подключить"_q), start);
		box->addButton(rpl::single(u"Закрыть"_q), [=] { box->closeBox(); });
		box->addLeftButton(rpl::single(u"Отключить VLESS"_q), [&proxy] {
			proxy.stop();
			Core::App().setVlessProxy(std::nullopt);
		});
		box->setFocusCallback([=] { field->setFocusFast(); });
	}), Ui::LayerOption::KeepOther);
}

} // namespace Ovygram
