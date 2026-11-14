#pragma once

#include "settings/settings_type.h"

namespace Settings {

[[nodiscard]] Type PluginsId();
[[nodiscard]] bool IsPluginsSection(const Type &type);

} // namespace Settings
