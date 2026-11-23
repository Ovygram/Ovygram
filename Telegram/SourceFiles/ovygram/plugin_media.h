#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <map>
#include <memory>

class QIODevice;
namespace Main { class Session; }

namespace Ovygram {

// The native picker can use a Qt fallback. Suspend plugin access to its
// widget tree and input until the platform dialog returns.
[[nodiscard]] bool PluginFilePickerActive();

class PluginMedia final : public QObject {
public:
	PluginMedia();
	~PluginMedia();

	using Result = Fn<void(QJsonValue)>;
	using Error = Fn<void(QString)>;
	void pick(not_null<Main::Session*> session, QString installationId, QString name,
		Fn<bool()> pending, Result reply, Error fail);
	QJsonValue request(not_null<Main::Session*> session, const QJsonObject &command);
	void remove(const QString &installationId);

private:
	struct Grant;
	std::map<QString, std::unique_ptr<Grant>> _grants;
};

} // namespace Ovygram
