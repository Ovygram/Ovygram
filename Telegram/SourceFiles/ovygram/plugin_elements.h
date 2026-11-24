#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

#include <functional>
#include <initializer_list>
#include <utility>

class QWidget;
class PeerData;
namespace Ui { class InputField; }

namespace Ovygram {

// Called by Telegram widgets (the only upstream-facing part).
//
// Gives built-in widgets stable names, so plugins can hide them or handle
// their clicks. A name can belong to several widgets, one per window.
void RegisterElements(
	std::initializer_list<std::pair<const char*, QWidget*>> elements);

// Called after the owner set the visibility of its children and before it
// lays them out, so hidden elements leave no gap.
void ApplyElementRules(QWidget *owner);

// Editor operations are scoped to a field, account and chat. The callbacks
// belong to the field and are discarded as soon as it is destroyed.
void RegisterEditor(Ui::InputField *field, QString kind,
	std::function<PeerData*()> peer, std::function<QJsonObject()> snapshot,
	std::function<bool(const QJsonObject&)> action);

// Called by PluginUi.
namespace Elements {

void SetEventSender(std::function<void(QJsonObject)> send);
void Handle(const QJsonObject &command);

// Requests with a reply: "compose.get_text".
[[nodiscard]] QJsonValue Request(const QJsonObject &command);
void RemovePlugin(const QString &installationId);

// Positions in the text of a field: QTextDocument counts UTF-16 code units,
// the Python side code points.
[[nodiscard]] int UnitsToCodePoints(const QString &text, int units);
[[nodiscard]] int CodePointsToUnits(const QString &text, int points);

} // namespace Elements
} // namespace Ovygram
