#include "ovygram/i18n.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

namespace Ovygram::I18n {
namespace {

struct State {
	std::mutex mutex;
	QString language = QStringLiteral("en");
	QString baseLanguage;
	std::uint64_t nextId = 1;
	std::map<std::uint64_t, std::function<void()>> listeners;
};

State &GlobalState() {
	static auto state = State();
	return state;
}

bool IsValidLangChar(QChar ch) {
	const auto u = ch.unicode();
	return (u >= 'a' && u <= 'z')
		|| (u >= '0' && u <= '9')
		|| (u == '-');
}

QString LookupTranslation(
		const std::map<QString, QString> &normalized,
		const QString &code) {
	if (code.isEmpty()) {
		return {};
	}
	if (const auto it = normalized.find(code); it != normalized.end()) {
		return it->second;
	}
	const auto primary = PrimaryLanguage(code);
	if (primary != code) {
		if (const auto it = normalized.find(primary); it != normalized.end()) {
			return it->second;
		}
	}
	return {};
}

} // namespace

QString NormalizeLanguage(const QString &code) {
	auto text = code.trimmed().toLower();
	if (text.isEmpty() || text.startsWith('#')) {
		return {};
	}
	text.replace('_', '-');
	for (const auto ch : text) {
		if (!IsValidLangChar(ch)) {
			return {};
		}
	}
	while (text.startsWith('-')) {
		text.remove(0, 1);
	}
	while (text.endsWith('-')) {
		text.chop(1);
	}
	if (text.isEmpty() || text.size() > 32) {
		return {};
	}
	return text;
}

QString PrimaryLanguage(const QString &code) {
	const auto normalized = NormalizeLanguage(code);
	const auto dash = normalized.indexOf('-');
	return (dash > 0) ? normalized.left(dash) : normalized;
}

void SetLanguage(const QString &language, const QString &baseLanguage) {
	auto normLang = NormalizeLanguage(language);
	auto normBase = NormalizeLanguage(baseLanguage);
	if (normLang.isEmpty()) {
		normLang = normBase.isEmpty() ? QStringLiteral("en") : normBase;
	}
	if (normBase == normLang) {
		normBase.clear();
	}

	auto callbacks = std::vector<std::function<void()>>();
	{
		auto &state = GlobalState();
		const auto lock = std::lock_guard(state.mutex);
		if (state.language == normLang && state.baseLanguage == normBase) {
			return;
		}
		state.language = normLang;
		state.baseLanguage = normBase;
		callbacks.reserve(state.listeners.size());
		for (const auto &[id, fn] : state.listeners) {
			if (fn) {
				callbacks.push_back(fn);
			}
		}
	}
	for (const auto &callback : callbacks) {
		callback();
	}
}

QString Language() {
	auto &state = GlobalState();
	const auto lock = std::lock_guard(state.mutex);
	return state.language;
}

QString BaseLanguage() {
	auto &state = GlobalState();
	const auto lock = std::lock_guard(state.mutex);
	return state.baseLanguage;
}

bool IsRussian() {
	auto &state = GlobalState();
	const auto lock = std::lock_guard(state.mutex);
	return PrimaryLanguage(state.language) == QStringLiteral("ru")
		|| PrimaryLanguage(state.baseLanguage) == QStringLiteral("ru");
}

QString UiLanguage() {
	return IsRussian() ? QStringLiteral("ru") : QStringLiteral("en");
}

std::uint64_t RegisterListener(std::function<void()> callback) {
	auto &state = GlobalState();
	const auto lock = std::lock_guard(state.mutex);
	const auto id = state.nextId++;
	state.listeners.emplace(id, std::move(callback));
	return id;
}

void UnregisterListener(std::uint64_t id) {
	auto &state = GlobalState();
	const auto lock = std::lock_guard(state.mutex);
	state.listeners.erase(id);
}

QString PickLocalized(
		const QString &fallback,
		const QJsonObject &translations) {
	if (translations.isEmpty()) {
		return fallback;
	}
	auto normalized = std::map<QString, QString>();
	for (auto it = translations.constBegin(); it != translations.constEnd(); ++it) {
		if (!it.value().isString()) {
			continue;
		}
		const auto key = NormalizeLanguage(it.key());
		const auto value = it.value().toString();
		if (!key.isEmpty() && !value.trimmed().isEmpty()) {
			normalized.emplace(key, value);
		}
	}
	if (normalized.empty()) {
		return fallback;
	}

	QString active;
	QString base;
	{
		auto &state = GlobalState();
		const auto lock = std::lock_guard(state.mutex);
		active = state.language;
		base = state.baseLanguage;
	}

	if (const auto hit = LookupTranslation(normalized, active); !hit.isEmpty()) {
		return hit;
	}
	if (const auto hit = LookupTranslation(normalized, base); !hit.isEmpty()) {
		return hit;
	}
	if (const auto hit = LookupTranslation(normalized, QStringLiteral("en")); !hit.isEmpty()) {
		return hit;
	}
	for (const auto &[key, value] : normalized) {
		if (PrimaryLanguage(key) == QStringLiteral("en")) {
			return value;
		}
	}
	if (!fallback.trimmed().isEmpty()) {
		return fallback;
	}
	return normalized.begin()->second;
}

QString PluginName(const QJsonObject &object) {
	const auto fallback = object.value(QStringLiteral("name")).toString();
	const auto localized = object.value(QStringLiteral("name_i18n")).toObject();
	return PickLocalized(fallback, localized);
}

QString PluginDescription(const QJsonObject &object) {
	const auto fallback = object.value(QStringLiteral("description")).toString();
	const auto localized = object.value(QStringLiteral("description_i18n")).toObject();
	return PickLocalized(fallback, localized);
}

} // namespace Ovygram::I18n

namespace Ovygram {

QString Tr(QStringView en, QStringView ru) {
	return I18n::IsRussian() ? ru.toString() : en.toString();
}

QString TrCount(
		int count,
		QStringView enOne,
		QStringView enOther,
		QStringView ruOne,
		QStringView ruFew,
		QStringView ruMany) {
	const auto absCount = std::abs(count);
	if (!I18n::IsRussian()) {
		return (absCount == 1 ? enOne : enOther).toString();
	}
	const auto mod100 = absCount % 100;
	const auto mod10 = absCount % 10;
	if (mod10 == 1 && mod100 != 11) {
		return ruOne.toString();
	}
	if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) {
		return ruFew.toString();
	}
	return ruMany.toString();
}

} // namespace Ovygram
