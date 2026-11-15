#include "ovygram/plugin_updates.h"

#include "rpl/event_stream.h"

namespace Ovygram {
namespace {

struct State {
	int count = 0;
	QString key;
	Fn<void()> dismiss;
	rpl::event_stream<> changes;
};

State &Get() {
	// Shared by account windows, including those created before PluginClient.
	static const auto state = new State();
	return *state;
}

} // namespace

void SetPluginUpdateNotification(const QJsonObject &notification) {
	auto &state = Get();
	const auto count = notification.value(u"count"_q).toInt();
	const auto key = notification.value(u"key"_q).toString();
	if (state.count != count || state.key != key) {
		state.count = count;
		state.key = key;
		state.changes.fire({});
	}
}

void SetPluginUpdateDismissCallback(Fn<void()> callback) {
	Get().dismiss = std::move(callback);
}

void DismissPluginUpdateNotification() {
	if (const auto callback = Get().dismiss) {
		callback();
	}
}

int PluginUpdateCount() {
	return Get().count;
}

rpl::producer<> PluginUpdateChanges() {
	return Get().changes.events();
}

} // namespace Ovygram
