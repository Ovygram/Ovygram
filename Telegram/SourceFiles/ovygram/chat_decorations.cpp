#include "ovygram/chat_decorations.h"

#include "ovygram/plugin_paint.h"

#include "data/data_peer.h"
#include "main/main_session.h"
#include "core/application.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "data/data_session.h"
#include "history/history.h"

#include <QtCore/QJsonArray>
#include <QtGui/QColor>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <cstdlib>
#include <map>
#include <typeinfo>
#include <stdexcept>
#include <vector>

#ifndef _MSC_VER
#include <cxxabi.h>
#endif // !_MSC_VER

namespace Ovygram {
namespace {

struct Decoration {
	QString installationId;
	QString accountId;
	QString chatId; // "*" decorates every chat of the account.
	int footerHeight = 0;
	QColor background;
	std::vector<PaintOp> ops;
};

struct State {
	std::map<QString, Decoration> decorations; // Decoration ID -> decoration.
};

State &Get() {
	static auto result = State();
	return result;
}

[[nodiscard]] bool IsChatList(QWidget *widget) {
	const auto raw = typeid(*widget).name();
#ifdef _MSC_VER
	return QLatin1String(raw).endsWith(QLatin1String("Dialogs::InnerWidget"));
#else // _MSC_VER
	auto status = 0;
	const auto demangled = abi::__cxa_demangle(raw, nullptr, nullptr, &status);
	const auto result = (status == 0 && demangled)
		&& (QLatin1String(demangled) == QLatin1String("Dialogs::InnerWidget"));
	std::free(demangled);
	return result;
#endif // _MSC_VER
}

void RefreshHeight(const Decoration &decoration) {
	if (!decoration.footerHeight) return;
	for (const auto &entry : Core::App().domain().accounts()) {
		const auto session = entry.account->maybeSession();
		if (!session || QString::number(session->uniqueId()) != decoration.accountId) continue;
		if (const auto history = session->data().historyLoaded(PeerId(decoration.chatId.toULongLong()))) {
			history->updateChatListEntryHeight();
		}
	}
}

void RepaintChatLists() {
	for (const auto widget : QApplication::allWidgets()) {
		if (widget->isVisible() && IsChatList(widget)) {
			widget->update();
		}
	}
}

} // namespace

int ChatFooterHeight(PeerData *peer) {
	const auto &rules = Get().decorations;
	if (rules.empty()) return 0;
	if (!peer) return 0;
	const auto accountId = QString::number(peer->session().uniqueId());
	const auto chatId = QString::number(peer->id.value);
	auto height = 0;
	for (const auto &[id, decoration] : rules) {
		if (decoration.accountId == accountId && decoration.chatId == chatId) {
			height += decoration.footerHeight;
		}
	}
	return height;
}

void PaintChatRow(QPainter &p, PeerData *peer, QRect row, bool active) {
	const auto &decorations = Get().decorations;
	if (!peer || decorations.empty()) {
		return;
	}
	const auto accountId = QString::number(peer->session().uniqueId());
	const auto chatId = QString::number(peer->id.value);
	p.save();
	p.setRenderHint(QPainter::Antialiasing);
	auto nextFooter = row.height() - ChatFooterHeight(peer);
	for (const auto &[id, decoration] : decorations) {
		if (decoration.accountId != accountId
			|| (decoration.chatId != chatId && decoration.chatId != u"*"_q)) {
			continue;
		}
		// The active row has its own strong background; keep it readable.
		auto area = row;
		if (decoration.footerHeight) {
			area = QRect(area.x(), nextFooter, area.width(), decoration.footerHeight);
			nextFooter += decoration.footerHeight;
		}
		p.save();
		if (decoration.footerHeight) p.setClipRect(area, Qt::IntersectClip);
		if (decoration.background.isValid() && !active) {
			p.fillRect(area, decoration.background);
		}
		PaintOps(p, decoration.ops, area);
		p.restore();
	}
	p.restore();
}

namespace Decorations {

void Handle(const QJsonObject &command) {
	const auto method = command.value(u"method"_q).toString();
	const auto id = command.value(u"decoration_id"_q).toString();
	if (id.isEmpty()) {
		throw std::runtime_error("A decoration needs an ID.");
	}
	auto &decorations = Get().decorations;
	const auto owner = command.value(u"installation_id"_q).toString();
	const auto existing = decorations.find(id);
	if (owner.isEmpty() || (existing != end(decorations) && existing->second.installationId != owner)) {
		throw std::runtime_error("Decoration owner mismatch.");
	}
	if (method == u"ui.decoration.set"_q) {
		auto decoration = Decoration{
			.installationId = command.value(u"installation_id"_q).toString(),
			.accountId = command.value(u"account_id"_q).toString(),
			.chatId = command.value(u"chat_id"_q).toString(),
			.footerHeight = command.value(u"footer_height"_q).toInt(),
			.ops = ParsePaintOps(command.value(u"commands"_q).toArray()),
		};
		if (decoration.accountId.isEmpty() || decoration.chatId.isEmpty()) {
			throw std::runtime_error("A decoration needs account_id and chat_id.");
		}
		if (decoration.footerHeight < 0 || decoration.footerHeight > 256
			|| (decoration.footerHeight && decoration.chatId == u"*"_q)) {
			throw std::runtime_error("A footer requires one target and height 1..256.");
		}
		if (decoration.footerHeight && existing == end(decorations) && decorations.size() >= 4096) {
			throw std::runtime_error("Too many decorations.");
		}
		const auto background = command.value(u"background"_q);
		if (!background.isNull() && !background.isUndefined()) {
			decoration.background = ParsePaintColor(background);
		}
		if (existing != end(decorations)) {
			const auto old = std::move(existing->second);
			decorations.erase(existing);
			RefreshHeight(old);
		}
		RefreshHeight(decorations[id] = std::move(decoration));
	} else if (method == u"ui.decoration.remove"_q) {
		if (existing != end(decorations)) {
			const auto old = std::move(existing->second);
			decorations.erase(existing);
			RefreshHeight(old);
		}
	} else {
		throw std::runtime_error("Unknown decoration command.");
	}
	RepaintChatLists();
}

void RemovePlugin(const QString &installationId) {
	auto removed = std::vector<Decoration>();
	std::erase_if(Get().decorations, [&](auto &entry) {
		if (entry.second.installationId != installationId) return false;
		removed.push_back(entry.second);
		return true;
	});
	for (const auto &decoration : removed) RefreshHeight(decoration);
	if (!removed.empty()) {
		RepaintChatLists();
	}
}

} // namespace Decorations
} // namespace Ovygram
