#pragma once

#include "mtproto/mtproto_proxy_data.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QProcess>
#include <QtCore/QTimer>
#include <QtNetwork/QTcpSocket>
#include <rpl/variable.h>

namespace Ovygram {

class VlessProxy final : public QObject {
public:
	VlessProxy();
	~VlessProxy() override;

	[[nodiscard]] QString start(const QString &link);
	void stop();
	[[nodiscard]] rpl::producer<QString> status() const;

private:
	void probe();
	void probeReply();
	void fail(QString message);

	QProcess *_process = nullptr;
	QTimer _poll;
	QElapsedTimer _started;
	QTcpSocket _probe;
	MTP::ProxyData _proxy;
	bool _authenticating = false;
	rpl::variable<QString> _status;
};

[[nodiscard]] bool IsVlessLink(const QString &link);
void ShowVlessBox(const QString &link = {});

} // namespace Ovygram
