#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QString>
#include <QtCore/QStringView>

#include <cstdint>
#include <functional>

namespace Ovygram::I18n {

[[nodiscard]] QString NormalizeLanguage(const QString &code);
[[nodiscard]] QString PrimaryLanguage(const QString &code);

void SetLanguage(const QString &language, const QString &baseLanguage = {});

[[nodiscard]] QString Language();
[[nodiscard]] QString BaseLanguage();
[[nodiscard]] QString UiLanguage();
[[nodiscard]] bool IsRussian();

[[nodiscard]] std::uint64_t RegisterListener(std::function<void()> callback);
void UnregisterListener(std::uint64_t id);

[[nodiscard]] QString PickLocalized(
	const QString &fallback,
	const QJsonObject &translations);
[[nodiscard]] QString PluginName(const QJsonObject &object);
[[nodiscard]] QString PluginDescription(const QJsonObject &object);

} // namespace Ovygram::I18n

namespace Ovygram {

[[nodiscard]] QString Tr(QStringView en, QStringView ru);
[[nodiscard]] QString TrCount(
	int count,
	QStringView enOne,
	QStringView enOther,
	QStringView ruOne,
	QStringView ruFew,
	QStringView ruMany);

} // namespace Ovygram
