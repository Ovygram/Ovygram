#include "ovygram/plugin_search.h"

#include "dialogs/dialogs_widget.h"
#include "ovygram/plugin_widgets.h"
#include "ovygram/protected_ui.h"

#include <QtCore/QPointer>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <rpl/event_stream.h>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace Ovygram::Search {
namespace {

struct Rule {
	QString owner;
	QPointer<QWidget> view;
	QString account;
	QString chat;
	Query query;
	bool local = false;
	QMetaObject::Connection destroyed;
};

struct Batch {
	QString rule;
	std::function<void(const QJsonArray&)> reply;
};

struct State {
	std::map<QString, Rule> rules;
	std::map<QString, Batch> batches;
	std::function<void(QJsonObject)> send;
	rpl::event_stream<Scope> changes;
	rpl::event_stream<Scope> more;
};

State &Get() {
	static const auto state = new State();
	return *state;
}

bool Matches(const Rule &rule, const Scope &scope) {
	return rule.view == scope.view && rule.account == scope.account && rule.chat == scope.chat;
}

void Remove(const QString &id, bool targetDestroyed = false) {
	const auto found = Get().rules.find(id);
	if (found == Get().rules.end()) return;
	const auto rule = found->second;
	QObject::disconnect(rule.destroyed);
	Get().rules.erase(found);
	std::erase_if(Get().batches, [&](const auto &entry) { return entry.second.rule == id; });
	if (targetDestroyed) {
		QTimer::singleShot(0, [id, rule] {
			if (Get().send) Get().send({
				{ u"installation_id"_q, rule.owner }, { u"type"_q, u"search_closed"_q },
				{ u"rule_id"_q, id }, { u"value"_q, QJsonObject{ { u"closed"_q, true } } },
			});
			if (rule.view) Get().changes.fire({ rule.view.data(), rule.account, rule.chat });
		});
	} else if (rule.view) {
		Get().changes.fire({ rule.view.data(), rule.account, rule.chat });
	}
}

void Reply(const QString &token, const QJsonArray &hidden) {
	const auto found = Get().batches.find(token);
	if (found == Get().batches.end()) return;
	const auto reply = std::move(found->second.reply);
	Get().batches.erase(found);
	reply(hidden);
}

} // namespace

void SetEventSender(std::function<void(QJsonObject)> send) {
	Get().send = std::move(send);
	if (!Get().send) {
		for (const auto &entry : Get().rules) {
			QObject::disconnect(entry.second.destroyed);
		}
		Get().rules.clear();
		Get().batches.clear();
	}
}

rpl::producer<Scope> Changes() { return Get().changes.events(); }
rpl::producer<Scope> ContinueRequests() { return Get().more.events(); }

bool HasPredicate(const Scope &scope) {
	return std::any_of(Get().rules.begin(), Get().rules.end(),
		[&](const auto &entry) { return entry.second.local && Matches(entry.second, scope); });
}

Query Parameters(const Scope &scope) {
	auto query = Query();
	for (const auto &[id, rule] : Get().rules) {
		if (!Matches(rule, scope)) continue;
		query.minDate = std::max(query.minDate, rule.query.minDate);
		if (rule.query.maxDate && (!query.maxDate || rule.query.maxDate < query.maxDate)) {
			query.maxDate = rule.query.maxDate;
		}
		if (!rule.query.media.isEmpty()) {
			if (!query.media.isEmpty() && query.media != rule.query.media) query.conflict = true;
			query.media = rule.query.media;
		}
	}
	if (query.maxDate && query.minDate > query.maxDate) query.conflict = true;
	return query;
}

void Handle(const QJsonObject &command) {
	const auto method = command.value(u"method"_q).toString();
	const auto owner = command.value(u"installation_id"_q).toString();
	const auto id = command.value(u"rule_id"_q).toString();
	const auto found = Get().rules.find(id);
	if (id.isEmpty() || owner.isEmpty() || (found != end(Get().rules) && found->second.owner != owner)) {
		throw std::runtime_error("Invalid search filter owner.");
	}
	if (method == u"ui.search.add"_q) {
		if (found != end(Get().rules) || Get().rules.size() >= 64) {
			throw std::runtime_error("Too many filters or duplicate search filter.");
		}
		const auto target = Widgets::Resolve(command.value(u"target"_q));
		auto view = target;
		while (view && !dynamic_cast<Dialogs::Widget*>(view)) view = view->parentWidget();
		const auto dialog = view ? static_cast<Dialogs::Widget*>(view) : nullptr;
		if (!dialog || target != dialog->pluginSearchTarget() || !target->isVisible()
				|| ProtectedFromPlugins(target)) {
			throw std::runtime_error("No accessible search filter row.");
		}
		const auto scope = dialog->pluginSearchScope();
		if (scope.account != command.value(u"account_id"_q).toString()
				|| scope.chat != command.value(u"chat_id"_q).toString()) {
			throw std::runtime_error("The search context has changed.");
		}
		const auto min = command.value(u"min_date"_q).toInt(-1);
		const auto max = command.value(u"max_date"_q).toInt(-1);
		const auto media = command.value(u"media"_q).toString();
		if (min < 0 || max < 0 || (max && min > max)
				|| (!media.isEmpty() && media != u"photo"_q && media != u"video"_q
					&& media != u"document"_q && media != u"url"_q && media != u"voice"_q)) {
			throw std::runtime_error("Invalid search parameters.");
		}
		auto &rule = Get().rules.emplace(id, Rule{owner, view, scope.account, scope.chat, {min, max, media},
			command.value(u"local"_q).toBool()}).first->second;
		rule.destroyed = QObject::connect(target, &QObject::destroyed, [id] { Remove(id, true); });
		Get().changes.fire_copy(scope);
	} else if (found == end(Get().rules)) {
		return;
	} else if (method == u"ui.search.remove"_q) {
		Remove(id);
	} else if (method == u"ui.search.more"_q) {
		const auto &rule = found->second;
		Get().more.fire({ rule.view.data(), rule.account, rule.chat });
	} else if (method == u"ui.search.result"_q) {
		const auto token = command.value(u"token"_q).toString();
		const auto batch = Get().batches.find(token);
		if (batch != end(Get().batches) && batch->second.rule == id) {
			Reply(token, command.value(u"hidden"_q).toArray());
		}
	}
}

void RemovePlugin(const QString &owner) {
	auto ids = std::vector<QString>();
	for (const auto &[id, rule] : Get().rules) if (rule.owner == owner) ids.push_back(id);
	for (const auto &id : ids) Remove(id);
}

void Filter(const Scope &scope, const QJsonArray &rows, std::function<void(std::vector<bool>)> done) {
	struct Page {
		std::vector<bool> visible;
		int pending = 0;
	};
	auto page = std::make_shared<Page>();
	page->visible.assign(rows.size(), true);
	auto events = std::vector<QJsonObject>();
	for (const auto &[id, rule] : Get().rules) {
		if (!rule.local || !Matches(rule, scope)) continue;
		const auto token = QUuid::createUuid().toString();
		++page->pending;
		Get().batches.emplace(token, Batch{id, [=](const QJsonArray &hidden) {
			for (const auto &value : hidden) {
				const auto index = value.toInt(-1);
				if (index >= 0 && index < int(page->visible.size())) page->visible[index] = false;
			}
			if (!--page->pending) done(std::move(page->visible));
		}});
		QTimer::singleShot(5000, [=] { Reply(token, {}); });
		events.push_back({
			{ u"installation_id"_q, rule.owner }, { u"type"_q, u"search_page"_q },
			{ u"rule_id"_q, id }, { u"value"_q, QJsonObject{
				{ u"token"_q, token }, { u"rows"_q, rows }
			} }
		});
	}
	if (events.empty()) {
		done(std::move(page->visible));
		return;
	}
	for (const auto &event : events) if (Get().send) Get().send(event);
}

} // namespace Ovygram::Search
