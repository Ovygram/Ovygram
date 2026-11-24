#pragma once

#include <QtCore/QJsonObject>
#include <functional>
#include <rpl/producer.h>

namespace Platform { class Tray; }
namespace Window { class SessionController; }

namespace Ovygram::Actions {

void SetEventSender(std::function<void(QJsonObject)> send);
void Register(const QJsonObject &command);
void Remove(const QString &owner, const QString &id);
void RemovePlugin(const QString &owner);
[[nodiscard]] bool ConfigureOwned(const QString &owner, const QString &id);
void FillTray(Platform::Tray &tray);
[[nodiscard]] rpl::producer<> TrayChanges();
void OpenLink(Window::SessionController *controller, const QString &owner, const QString &name);

} // namespace Ovygram::Actions
