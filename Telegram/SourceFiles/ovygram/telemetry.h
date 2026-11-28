#pragma once

#include <QtCore/QString>

namespace Ovygram::Telemetry {

void Start(bool confirmedPreviousCrash);
void Finish();
void WindowOpened();
void PluginHostStarted();
void PluginHostFailed();

[[nodiscard]] bool Enabled();
void SetEnabled(bool enabled);
[[nodiscard]] QString InstallationId();
[[nodiscard]] QString SupportDetails();

} // namespace Ovygram::Telemetry
