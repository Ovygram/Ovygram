#pragma once

#include <QtCore/QString>

namespace Ui {
class GenericBox;
class VerticalLayout;
} // namespace Ui

namespace Ovygram::Telemetry {

void AddDiagnosticsRows(
	not_null<Ui::VerticalLayout*> layout,
	QString id,
	bool enabled,
	Fn<void(bool)> setEnabled,
	Fn<void()> preview);
void BuildSupportBox(not_null<Ui::GenericBox*> box, const QString &details);

} // namespace Ovygram::Telemetry
