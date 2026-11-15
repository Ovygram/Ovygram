#pragma once

#include "rpl/producer.h"

#include <QtCore/QJsonObject>

namespace Ovygram {

void SetPluginUpdateNotification(const QJsonObject &notification);
void SetPluginUpdateDismissCallback(Fn<void()> callback);
void DismissPluginUpdateNotification();
[[nodiscard]] int PluginUpdateCount();
[[nodiscard]] rpl::producer<> PluginUpdateChanges();

} // namespace Ovygram
