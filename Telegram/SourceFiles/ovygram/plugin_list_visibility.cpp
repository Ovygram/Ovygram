#include "ovygram/plugin_list_visibility.h"

#include "data/data_peer.h"
#include "main/main_session.h"

#include <QtCore/QJsonArray>
#include <rpl/event_stream.h>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace Ovygram::ListVisibility {
namespace {

constexpr auto kMaximumIds = 256;
constexpr auto kMaximumRules = 64;

struct Rule {
	QString owner;
	uint64 account = 0;
	uint64 chat = 0;
	List list = List::Members;
	std::set<uint64> ids;
};

struct State {
	std::map<QString, Rule> rules;
	rpl::event_stream<Scope> changes;
};

State &Get() {
	static const auto state = new State();
	return *state;
}

uint64 ReadId(const QJsonValue &value) {
	auto valid = false;
	const auto number = value.toString().toULongLong(&valid);
	if (!valid || !number) throw std::runtime_error("Invalid list entity ID.");
	return number;
}

bool Applies(const Rule &rule, List list, not_null<PeerData*> chat) {
	return rule.list == list
		&& rule.chat == chat->id.value
		&& rule.account == chat->session().uniqueId();
}

} // namespace

rpl::producer<Scope> Changes() {
	return Get().changes.events();
}

bool Hidden(List list, not_null<PeerData*> chat, uint64 id) {
	return std::any_of(Get().rules.begin(), Get().rules.end(), [&](const auto &entry) {
		return Applies(entry.second, list, chat) && entry.second.ids.contains(id);
	});
}

int MediaAllowance(not_null<PeerData*> chat) {
	auto count = 0;
	for (const auto &[id, rule] : Get().rules) {
		if (Applies(rule, List::Media, chat)) count += int(rule.ids.size());
	}
	return count;
}

void Handle(const QJsonObject &command) {
	auto &state = Get();
	const auto id = command.value(u"rule_id"_q).toString();
	const auto owner = command.value(u"installation_id"_q).toString();
	const auto existing = state.rules.find(id);
	if (id.isEmpty() || owner.isEmpty()
		|| (existing != state.rules.end() && existing->second.owner != owner)) {
		throw std::runtime_error("Invalid list rule owner.");
	}
	const auto method = command.value(u"method"_q).toString();
	if (method == u"ui.list_visibility.remove"_q) {
		if (existing != state.rules.end()) {
			const auto &rule = existing->second;
			const auto scope = Scope{ rule.list, rule.account, rule.chat };
			state.rules.erase(existing);
			state.changes.fire_copy(scope);
		}
		return;
	}
	if (method != u"ui.list_visibility.add"_q) {
		throw std::runtime_error("Unknown list visibility command.");
	}
	const auto kind = command.value(u"list"_q).toString();
	if (kind != u"members"_q && kind != u"media"_q) {
		throw std::runtime_error("Invalid list type.");
	}
	const auto values = command.value(u"ids"_q).toArray();
	if (values.isEmpty() || values.size() > kMaximumIds
		|| state.rules.size() >= kMaximumRules || existing != state.rules.end()) {
		throw std::runtime_error("Invalid list rule size or duplicate rule.");
	}
	auto rule = Rule{
		.owner = owner,
		.account = ReadId(command.value(u"account_id"_q)),
		.chat = ReadId(command.value(u"chat_id"_q)),
		.list = kind == u"members"_q ? List::Members : List::Media,
	};
	for (const auto &value : values) rule.ids.insert(ReadId(value));
	if (rule.list == List::Media) {
		auto total = int(rule.ids.size());
		for (const auto &[key, previous] : state.rules) {
			if (previous.account == rule.account && previous.chat == rule.chat && previous.list == List::Media) {
				total += int(previous.ids.size());
			}
		}
		if (total > kMaximumIds) throw std::runtime_error("Too many hidden media items for this chat.");
	}
	const auto scope = Scope{ rule.list, rule.account, rule.chat };
	state.rules.emplace(id, std::move(rule));
	state.changes.fire_copy(scope);
}

void RemovePlugin(const QString &owner) {
	auto scopes = std::set<std::tuple<List, uint64, uint64>>();
	std::erase_if(Get().rules, [&](const auto &entry) {
		if (entry.second.owner != owner) return false;
		scopes.emplace(entry.second.list, entry.second.account, entry.second.chat);
		return true;
	});
	for (const auto &[list, account, chat] : scopes) Get().changes.fire({list, account, chat});
}

} // namespace Ovygram::ListVisibility
