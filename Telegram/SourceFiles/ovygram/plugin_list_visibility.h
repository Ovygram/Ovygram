#pragma once

#include <QtCore/QJsonObject>
#include <rpl/producer.h>

class PeerData;

namespace Ovygram::ListVisibility {

enum class List { Members, Media };

struct Scope {
	List list;
	uint64 account;
	uint64 chat;
};

void Handle(const QJsonObject &command);
void RemovePlugin(const QString &owner);
[[nodiscard]] rpl::producer<Scope> Changes();
[[nodiscard]] bool Hidden(List list, not_null<PeerData*> chat, uint64 id);
[[nodiscard]] int MediaAllowance(not_null<PeerData*> chat);

} // namespace Ovygram::ListVisibility
