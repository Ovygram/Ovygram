#include "ovygram/plugin_panel_buttons.h"

#include "ovygram/plugin_install.h"
#include "ovygram/plugin_menus.h"
#include "ovygram/plugin_widgets.h"
#include "ovygram/protected_ui.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "data/data_peer.h"
#include "dialogs/dialogs_widget.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include <QtGui/QAction>
#include "styles/style_layers.h"
#include "styles/style_widgets.h"
#include "styles/style_plugin_management.h"

#include <QtCore/QJsonArray>
#include <QtCore/QPointer>
#include <QtWidgets/QWidget>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace Ovygram::PanelButtons {
namespace {

constexpr auto kMaximumItems = 64;
constexpr auto kMaximumPerPlugin = 16;
constexpr auto kSpacing = 4;

struct Item {
	QString installationId;
	QString panel;
	QString text;
	QString tooltip;
	QString accountId;
	QString chatId;
	std::set<QString> kinds;
	int width = 40;
	int priority = 0;
	bool enabled = true;
	bool visible = true;
	quint64 sequence = 0;
};

struct Host {
	QPointer<QWidget> widget;
	QString panel;
	std::function<void()> relayout;
	std::map<QString, QPointer<Ui::RoundButton>> buttons;
	std::vector<QString> placed;
	std::vector<QString> overflow;
	QPointer<Ui::RoundButton> more;
	QPointer<Ui::PopupMenu> menu;
	bool showMore = false;
	base::weak_ptr<Main::Session> session;
	PeerId peerId;
};

struct State {
	std::map<QString, Item> items;
	std::map<QWidget*, std::unique_ptr<Host>> hosts;
	std::function<void(QJsonObject)> send;
	quint64 sequence = 0;
};

State &Get() {
	static const auto state = new State();
	return *state;
}

Host *Find(QWidget *owner) {
	const auto i = Get().hosts.find(owner);
	return i == end(Get().hosts) ? nullptr : i->second.get();
}

QString Kind(not_null<PeerData*> peer) {
	return peerIsChannel(peer->id) ? u"channel"_q
		: peerIsChat(peer->id) ? u"chat"_q : u"user"_q;
}

bool Matches(const Item &item, const Host &host, not_null<PeerData*> peer) {
	return item.visible && item.panel == host.panel
		&& (item.accountId.isEmpty() || item.accountId == QString::number(peer->session().uniqueId()))
		&& (item.chatId.isEmpty() || item.chatId == QString::number(peer->id.value))
		&& (item.kinds.empty() || item.kinds.contains(Kind(peer)));
}

void RefreshHosts() {
	// A layout can create/destroy widgets. Copy callbacks with their guards
	// before invoking them, so no map iterator survives a reentrant Qt callback.
	auto callbacks = std::vector<std::pair<QPointer<QWidget>, std::function<void()>>>();
	for (const auto &[owner, host] : Get().hosts) {
		callbacks.emplace_back(host->widget, host->relayout);
	}
	for (const auto &[widget, callback] : callbacks) {
		if (widget && callback) callback();
	}
}

void RemoveWidgets(const QString &id) {
	for (const auto &[owner, host] : Get().hosts) {
		if (const auto button = host->buttons.find(id); button != end(host->buttons)) {
			delete button->second.data();
			host->buttons.erase(button);
		}
		std::erase(host->placed, id);
	}
}

void Clicked(QWidget *owner, const QString &ruleId) {
	if (InstallationUiActive() || Core::App().passcodeLocked()
		|| ProtectedFromPlugins(owner)
		|| CouldCoverProtected(owner, u"geometry"_q)) {
		return;
	}
	const auto host = Find(owner);
	const auto i = Get().items.find(ruleId);
	if (!host || i == end(Get().items) || !Get().send || !i->second.enabled) return;
	const auto session = host->session.get();
	const auto peer = session ? session->data().peerLoaded(host->peerId) : nullptr;
	if (!peer || !Matches(i->second, *host, peer)) return;
	auto value = Menus::DescribeContext(*session, peer, nullptr);
	value.insert(u"panel"_q, host->panel);
	auto target = owner;
	if (host->panel == u"search.actions"_q) {
		const auto search = dynamic_cast<Dialogs::Widget*>(owner->parentWidget());
		target = search ? search->pluginSearchTarget() : nullptr;
		if (!target || !target->isVisible()) return;
	}
	value.insert(u"handle"_q, Widgets::HandleFor(target));
	Get().send(QJsonObject{
		{ u"installation_id"_q, i->second.installationId },
		{ u"type"_q, u"panel_button_click"_q },
		{ u"rule_id"_q, ruleId },
		{ u"value"_q, value },
	});
}

int Integer(const QJsonObject &command, const QString &key, int fallback, int minimum, int maximum) {
	const auto value = command.value(key);
	if (value.isUndefined()) return fallback;
	const auto number = value.toInt(minimum - 1);
	if (!value.isDouble() || value.toDouble() != number || number < minimum || number > maximum) {
		throw std::runtime_error("Invalid panel button integer: " + key.toStdString());
	}
	return number;
}

bool Boolean(const QJsonObject &command, const QString &key) {
	const auto value = command.value(key);
	if (!value.isUndefined() && !value.isBool()) throw std::runtime_error("Expected a boolean.");
	return value.toBool(true);
}

bool Decimal(const QString &value) {
	return !value.isEmpty() && std::all_of(value.begin(), value.end(), [](QChar character) {
		return character >= QChar('0') && character <= QChar('9');
	});
}

} // namespace

void RegisterHost(QWidget *owner, const QString &panel, std::function<void()> relayout) {
	if (Find(owner)) throw std::runtime_error("Panel host already registered.");
	auto host = std::make_unique<Host>();
	host->widget = owner;
	host->panel = panel;
	host->relayout = std::move(relayout);
	Get().hosts.emplace(owner, std::move(host));
	QObject::connect(owner, &QObject::destroyed, [owner] { Get().hosts.erase(owner); });
}

void UpdateProtectedSurfaces() {
	RefreshHosts();
}

void SetEventSender(std::function<void(QJsonObject)> send) {
	Get().send = std::move(send);
}

void Handle(const QJsonObject &command) {
	const auto method = command.value(u"method"_q).toString();
	const auto id = command.value(u"rule_id"_q).toString();
	const auto installationId = command.value(u"installation_id"_q).toString();
	if (id.isEmpty() || installationId.isEmpty()) throw std::runtime_error("Panel button needs an owner and ID.");
	const auto existing = Get().items.find(id);
	if (existing != end(Get().items) && existing->second.installationId != installationId) {
		throw std::runtime_error("Panel button belongs to another installation.");
	}
	if (method == u"ui.panel.remove"_q) {
		Get().items.erase(id);
		RemoveWidgets(id);
	} else if (method == u"ui.panel.add"_q) {
		auto item = Item{
			.installationId = installationId,
			.panel = command.value(u"panel"_q).toString(),
			.text = command.value(u"text"_q).toString().trimmed(),
			.tooltip = command.value(u"tooltip"_q).toString(),
			.accountId = command.value(u"account_id"_q).toString(),
			.chatId = command.value(u"chat_id"_q).toString(),
		};
		if (item.panel != u"chat.top_bar"_q && item.panel != u"compose"_q && item.panel != u"search.actions"_q) {
			throw std::runtime_error("Unknown panel button target.");
		}
		if (item.text.isEmpty() || item.text.size() > 32 || item.tooltip.size() > 256) {
			throw std::runtime_error("Panel button text/tooltip exceeds its limit.");
		}
		if ((!item.accountId.isEmpty() && !Decimal(item.accountId))
			|| (!item.chatId.isEmpty() && (!Decimal(item.chatId) || item.accountId.isEmpty()))) {
			throw std::runtime_error("Invalid panel button chat/account scope.");
		}
		item.width = Integer(command, u"width"_q, 40, 32, 160);
		item.priority = Integer(command, u"priority"_q, 0, -10000, 10000);
		item.enabled = Boolean(command, u"enabled"_q);
		item.visible = Boolean(command, u"visible"_q);
		for (const auto &kind : command.value(u"kinds"_q).toArray()) {
			const auto value = kind.toString();
			if (value != u"user"_q && value != u"chat"_q && value != u"channel"_q) {
				throw std::runtime_error("Invalid panel button chat kind.");
			}
			item.kinds.insert(value);
		}
		if (existing == end(Get().items)) {
			const auto own = std::count_if(Get().items.begin(), Get().items.end(), [&](const auto &entry) {
				return entry.second.installationId == installationId;
			});
			if (Get().items.size() >= kMaximumItems || own >= kMaximumPerPlugin) {
				throw std::runtime_error("Too many panel buttons.");
			}
			item.sequence = ++Get().sequence;
		} else {
			item.sequence = existing->second.sequence;
		}
		Get().items[id] = std::move(item);
	} else {
		throw std::runtime_error("Unknown panel button command.");
	}
	RefreshHosts();
}

void RemovePlugin(const QString &installationId) {
	auto removed = std::vector<QString>();
	for (const auto &[id, item] : Get().items) {
		if (item.installationId == installationId) removed.push_back(id);
	}
	for (const auto &id : removed) {
		Get().items.erase(id);
		RemoveWidgets(id);
	}
	RefreshHosts();
}

void Hide(QWidget *owner) {
	if (const auto host = Find(owner)) {
		if (const auto menu = std::exchange(host->menu, nullptr)) {
			menu->hideMenu(true);
		}
		host->placed.clear();
		host->overflow.clear();
		host->showMore = false;
		if (host->more) host->more->hide();
		host->session = {};
		for (const auto &[id, button] : host->buttons) {
			if (button) button->hide();
		}
	}
}

int Prepare(QWidget *owner, PeerData *peer, int availableWidth, int rowHeight) {
	const auto host = Find(owner);
	if (!host) return 0;
	Hide(owner);
	std::erase_if(host->buttons, [](const auto &entry) {
		if (Get().items.contains(entry.first)) return false;
		delete entry.second.data();
		return true;
	});
	if (!peer || availableWidth <= 0 || rowHeight <= 0
		|| InstallationUiActive() || Core::App().passcodeLocked()
		|| ProtectedFromPlugins(owner)
		|| CouldCoverProtected(owner, u"geometry"_q)) {
		return 0;
	}
	host->session = base::make_weak(&peer->session());
	host->peerId = peer->id;
	auto items = std::vector<std::pair<QString, Item>>();
	for (const auto &entry : Get().items) {
		if (Matches(entry.second, *host, peer)) items.push_back(entry);
	}
	std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
		return std::tuple(-a.second.priority, a.second.sequence) < std::tuple(-b.second.priority, b.second.sequence);
	});
	auto used = 0;
	for (const auto &[id, item] : items) {
		auto &button = host->buttons[id];
		if (!button) {
			button = new Ui::RoundButton(owner, rpl::single(item.text), st::settingsPluginPrimary);
			button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
			button->setClickedCallback([owner, id] { Clicked(owner, id); });
		}
		button->setText(rpl::single(item.text));
		button->setToolTip(item.tooltip);
		button->setAccessibleName(item.tooltip.isEmpty() ? item.text : item.tooltip);
		button->setDisabled(!item.enabled);
		button->setFullWidth(std::max(item.width, button->height()));
		const auto required = button->width() + kSpacing;
		if (button->height() > rowHeight || required > availableWidth - used) {
			host->overflow.push_back(id);
			continue;
		}
		host->placed.push_back(id);
		used += required;
	}
	if (host->overflow.empty()) return used;
	if (!host->more) {
		host->more = new Ui::RoundButton(owner, rpl::single(u"…"_q), st::settingsPluginPrimary);
		host->more->setFullWidth(host->more->height());
		host->more->setAccessibleName(u"Другие действия плагинов"_q);
		host->more->setFocusPolicy(Qt::StrongFocus);
		host->more->setClickedCallback([owner] {
			const auto host = Find(owner);
			if (!host || !host->showMore || host->menu
				|| InstallationUiActive() || Core::App().passcodeLocked()
				|| ProtectedSurfaceExists()) {
				return;
			}
			const auto account = host->session;
			const auto peer = host->peerId;
			host->menu = new Ui::PopupMenu(owner, st::settingsPluginPanelPopup);
			for (const auto &id : host->overflow) {
				const auto i = Get().items.find(id);
				if (i == end(Get().items)) continue;
				const auto action = host->menu->addAction(i->second.text, [=] {
					const auto current = Find(owner);
					if (current && current->showMore && !ProtectedSurfaceExists()
						&& current->session.get() == account.get()
						&& current->peerId == peer
						&& std::find(current->overflow.begin(), current->overflow.end(), id)
							!= current->overflow.end()) {
						Clicked(owner, id);
					}
				});
				action->setEnabled(i->second.enabled);
			}
			host->menu->popup(host->more->mapToGlobal(QPoint(0, host->more->height())));
		});
	}
	const auto required = host->more->width() + kSpacing;
	if (required > availableWidth || host->more->height() > rowHeight) return used;
	while (required > availableWidth - used && !host->placed.empty()) {
		const auto id = host->placed.back();
		used -= host->buttons[id]->width() + kSpacing;
		host->placed.pop_back();
	}
	host->overflow.clear();
	for (const auto &[id, item] : items) {
		if (std::find(host->placed.begin(), host->placed.end(), id) == host->placed.end()) {
			host->overflow.push_back(id);
		}
	}
	host->showMore = true;
	return used + required;
}

void Place(QWidget *owner, int left, int top, int rowHeight) {
	const auto host = Find(owner);
	if (!host) return;
	for (const auto &id : host->placed) {
		const auto button = host->buttons[id];
		if (!button) continue;
		button->moveToLeft(left, top + (rowHeight - button->height()) / 2, owner->width());
		button->show();
		left += button->width() + kSpacing;
	}
	if (host->showMore) {
		host->more->moveToLeft(left, top + (rowHeight - host->more->height()) / 2, owner->width());
		host->more->show();
	}
}

} // namespace Ovygram::PanelButtons
