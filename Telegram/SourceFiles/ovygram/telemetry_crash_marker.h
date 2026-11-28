#pragma once

#include <QtCore/QString>

namespace Ovygram::Telemetry::CrashMarker {

void Arm(const QString &path);
void Disarm();

} // namespace Ovygram::Telemetry::CrashMarker
