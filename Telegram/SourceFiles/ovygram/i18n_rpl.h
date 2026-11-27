#pragma once

#include "ovygram/i18n.h"

#include <rpl/producer.h>

namespace Ovygram::I18n {

[[nodiscard]] inline rpl::producer<QString> LanguageChanges() {
	return rpl::make_producer<QString>([](auto consumer) {
		const auto id = RegisterListener([consumer] {
			consumer.put_next(Language());
		});
		return rpl::lifetime([id] {
			UnregisterListener(id);
		});
	});
}

[[nodiscard]] inline rpl::producer<QString> LanguageValue() {
	return rpl::single(Language()) | rpl::then(LanguageChanges());
}

} // namespace Ovygram::I18n

namespace Ovygram {

[[nodiscard]] inline rpl::producer<QString> TrValue(QString en, QString ru) {
	return I18n::LanguageValue() | rpl::map([
		en = std::move(en),
		ru = std::move(ru)
	](const QString &) {
		return I18n::IsRussian() ? ru : en;
	});
}

} // namespace Ovygram
