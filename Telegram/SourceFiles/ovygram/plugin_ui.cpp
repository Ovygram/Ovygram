#include "ovygram/plugin_ui.h"
#include "window/notifications_manager.h"
#include <QtWidgets/QBoxLayout>
#include "ovygram/protected_ui.h"

#include "ovygram/chat_bars.h"
#include "ovygram/chat_decorations.h"
#include "ovygram/chat_list_filter.h"
#include "ovygram/message_decorations.h"
#include "ovygram/plugin_elements.h"
#include "ovygram/plugin_install.h"
#include "ovygram/plugin_menus.h"
#include "ovygram/plugin_actions.h"
#include "ovygram/plugin_panel_buttons.h"
#include "ovygram/plugin_native.h"
#include "ovygram/plugin_ui_tree.h"
#include "ovygram/plugin_section.h"
#include "ovygram/plugin_search.h"
#include "ovygram/plugin_list_visibility.h"
#include "ovygram/plugin_widgets.h"

#include "base/algorithm.h"
#include "base/unique_qptr.h"
#include "core/application.h"
#include "data/data_peer.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/rp_widget.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/scroll_area.h"
#include "ui/ui_utility.h"
#include "ui/widgets/separate_panel.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_controller.h"
#include "styles/style_layers.h"
#include "styles/style_plugin_management.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <QtCore/QPointer>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtGui/QPainter>
#include <QtGui/QResizeEvent>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Ovygram {
namespace {

constexpr auto kBarPadding = 6;

PluginUi *GlobalInstance = nullptr;

// Keeps an overlay canvas over its target widget.
class OverlayWatcher final : public QObject {
public:
	OverlayWatcher(QWidget *target, not_null<Ui::RpWidget*> canvas)
	: QObject(canvas)
	, _canvas(canvas.get()) {
		target->installEventFilter(this);
	}

protected:
	bool eventFilter(QObject *watched, QEvent *e) override {
		if (e->type() == QEvent::Resize && _canvas) {
			const auto size = static_cast<QResizeEvent*>(e)->size();
			PluginUiTree::ResizeCanvas(_canvas.data(), size);
		}
		return false;
	}

private:
	const QPointer<Ui::RpWidget> _canvas;

};

// Background and padding of a plugin bar inside the chat.
class BarFrame final : public Ui::RpWidget {
public:
	BarFrame(object_ptr<Ui::RpWidget> content)
	: RpWidget(nullptr)
	, _content(content.release()) {
		_content->setParent(this);
		_content->show();
	}

protected:
	int resizeGetHeight(int newWidth) override {
		const auto padding = st::boxRowPadding;
		_content->resizeToWidth(
			std::max(newWidth - padding.left() - padding.right(), 1));
		_content->moveToLeft(padding.left(), kBarPadding, newWidth);
		return _content->height() + 2 * kBarPadding;
	}

	void paintEvent(QPaintEvent *e) override {
		auto p = QPainter(this);
		p.fillRect(rect(), st::windowBg);
		p.fillRect(0, height() - st::lineWidth, width(), st::lineWidth, st::shadowFg);
	}

private:
	const QPointer<Ui::RpWidget> _content;

};

} // namespace

int LayoutChatBars(QWidget *chat, PeerData *peer, int left, int top, int width) {
	const auto instance = PluginUi::Instance();
	return instance
		? instance->layoutChatBars(chat, peer, left, top, width)
		: 0;
}

struct PluginUi::Object {
	QString installationId;
	QString objectId;
	QPointer<Ui::BoxContent> box;
	QPointer<Ui::RpWidget> inserted;
	std::unique_ptr<Ui::SeparatePanel> panel;
	base::unique_qptr<Ui::RpWidget> bar;
	base::unique_qptr<Ui::RpWidget> overlay;
	base::unique_qptr<Ui::RpWidget> section;
	QString title;
	QString accountId;
	QString chatId;
	PluginUiTree tree;
	bool silent = false;
};

PluginUi::PluginUi(EventSender send)
: _send(std::move(send)) {
	GlobalInstance = this;
	Elements::SetEventSender(_send);
	Widgets::SetEventSender(_send);
	Menus::SetEventSender(_send);
	Actions::SetEventSender(_send);
	PanelButtons::SetEventSender(_send);
	Search::SetEventSender(_send);
	Native::SetEventSender(_send);
	// A canvas over a widget that contains a protected surface would cover
	// it: such overlays stay hidden while the surface exists. A dialog of a
	// plugin is a layer above everything, so it is closed.
	ProtectedSurfacesChanged() | rpl::on_next([=] {
		PanelButtons::UpdateProtectedSurfaces();
		auto boxes = std::vector<QPointer<Ui::BoxContent>>();
		for (const auto &[id, object] : _objects) {
			if (const auto row = object->inserted.data()) {
				row->setVisible(!ProtectedFromPlugins(row->parentWidget())
					&& !CouldCoverProtected(row, u"geometry"_q));
			}
			if (const auto overlay = object->overlay.get()) {
				overlay->setVisible(
					!ProtectedFromPlugins(overlay->parentWidget()));
			}
			if (object->box) {
				boxes.push_back(object->box);
			}
		}
		if (ProtectedSurfaceExists()) {
			// Closing a box removes its object from the map.
			for (const auto &box : boxes) {
				if (box) {
					box->closeBox();
				}
			}
		}
	}, _lifetime);
}

PluginUi *PluginUi::Instance() {
	return GlobalInstance;
}

PluginUi::~PluginUi() {
	if (GlobalInstance == this) {
		GlobalInstance = nullptr;
		Elements::SetEventSender(nullptr);
		Widgets::SetEventSender(nullptr);
		Menus::SetEventSender(nullptr);
		Actions::SetEventSender(nullptr);
		PanelButtons::SetEventSender(nullptr);
		Search::SetEventSender(nullptr);
		Native::SetEventSender(nullptr);
	}
	// Surfaces report their destruction to destroyed(), which erases from
	// _objects. Disconnect them first, so destroying _objects can't reenter.
	auto objects = base::take(_objects);
	for (const auto &[id, object] : objects) {
		object->silent = true;
		delete object->inserted.data();
		if (object->section) {
			_sectionClosed.fire_copy(id);
		}
		for (const auto widget : std::initializer_list<QObject*>{
				object->bar.get(),
				object->overlay.get(),
				object->section.get(),
				object->box.data() }) {
			if (widget) {
				QObject::disconnect(widget, nullptr, this, nullptr);
			}
		}
		if (object->box) {
			// The box outlives us during its closing animation; its widgets
			// check the guard in build() before sending events.
			object->box->closeBox();
		}
	}
}

void PluginUi::handle(const QJsonObject &command) {
	const auto method = command.value(u"method"_q).toString();
	if (method == u"ui.close"_q || method == u"ui.update"_q) {
		const auto i = _objects.find(command.value(u"object_id"_q).toString());
		if (i != end(_objects) && i->second->installationId != command.value(u"installation_id"_q).toString()) {
			throw std::runtime_error("Surface belongs to another installation.");
		}
	}
	if (method == u"ui.show"_q) {
		show(command);
	} else if (method == u"ui.update"_q) {
		update(command);
	} else if (method == u"ui.close"_q) {
		close(command.value(u"object_id"_q).toString(), true);
	} else if (method == u"ui.close_plugin"_q) {
		closePlugin(command.value(u"installation_id"_q).toString());
		Elements::RemovePlugin(command.value(u"installation_id"_q).toString());
		Widgets::RemovePlugin(command.value(u"installation_id"_q).toString());
		Decorations::RemovePlugin(command.value(u"installation_id"_q).toString());
		Search::RemovePlugin(command.value(u"installation_id"_q).toString());
		ListVisibility::RemovePlugin(command.value(u"installation_id"_q).toString());
		ChatListFilter::RemovePlugin(command.value(u"installation_id"_q).toString());
		MessageDecorations::RemovePlugin(command.value(u"installation_id"_q).toString());
		Actions::RemovePlugin(command.value(u"installation_id"_q).toString());
		Menus::RemovePlugin(command.value(u"installation_id"_q).toString());
		PanelButtons::RemovePlugin(command.value(u"installation_id"_q).toString());
		Window::Notifications::RemovePluginNotificationRules(command.value(u"installation_id"_q).toString());
		Native::RemovePlugin(command.value(u"installation_id"_q).toString());
	} else if (method.startsWith(u"ui.list_visibility."_q)) {
		ListVisibility::Handle(command);
	} else if (method.startsWith(u"ui.search."_q)) {
		Search::Handle(command);
	} else if (method == u"ui.action.remove"_q) {
		Actions::Remove(command.value(u"installation_id"_q).toString(), command.value(u"rule_id"_q).toString());
	} else if (method == u"ui.notification.add"_q || method == u"ui.notification.remove"_q) {
		Window::Notifications::SetPluginNotificationRule(command);
	} else if (method.startsWith(u"ui.menu."_q)) {
		Menus::Handle(command);
	} else if (method.startsWith(u"ui.panel."_q)) {
		PanelButtons::Handle(command);
	} else if (method.startsWith(u"ui.native."_q)) {
		Native::Handle(command);
	} else if (method.startsWith(u"ui.message_decoration."_q)) {
		MessageDecorations::Handle(command);
	} else if (method.startsWith(u"ui.chat_list."_q)) {
		ChatListFilter::Handle(command);
	} else if (method.startsWith(u"ui.decoration."_q)) {
		Decorations::Handle(command);
	} else if (method.startsWith(u"ui.element."_q)
		|| method.startsWith(u"ui.compose."_q)
		|| method == u"ui.palette"_q) {
		Elements::Handle(command);
	} else if (method.startsWith(u"ui.widgets."_q) || method.startsWith(u"ui.keys."_q)) {
		Widgets::Handle(command);
	} else if (method == u"ui.clipboard"_q) {
		QGuiApplication::clipboard()->setText(command.value(u"text"_q).toString());
	} else {
		throw std::runtime_error("Unknown UI command.");
	}
}

void PluginUi::show(const QJsonObject &command) {
	const auto objectId = command.value(u"object_id"_q).toString();
	if (objectId.isEmpty() || _objects.contains(objectId)) {
		throw std::runtime_error("Invalid or repeated object ID.");
	}
	auto owned = std::make_unique<Object>();
	const auto object = owned.get();
	object->installationId = command.value(u"installation_id"_q).toString();
	object->objectId = objectId;
	const auto title = command.value(u"title"_q).toString();
	const auto tree = command.value(u"tree"_q).toObject();
	const auto kind = command.value(u"kind"_q).toString();
	if (kind == u"dialog"_q) {
		if (ProtectedSurfaceExists()) {
			// It would be drawn over the plugin manager with Telegram's own
			// components and could pass for a window of Ovygram.
			throw std::runtime_error(
				"Dialogs are unavailable while Ovygram's plugin manager is open.");
		}
		const auto window = Core::App().activePrimaryWindow();
		if (!window) {
			throw std::runtime_error("No active Telegram window.");
		}
		const auto width = command.value(u"width"_q).toInt();
		// Built before the box, so an invalid tree throws before showing.
		const auto content = std::make_shared<object_ptr<Ui::RpWidget>>(
			build(*object, nullptr, tree));
		const auto footer = command.value(u"footer_action"_q).toObject();
		if (!footer.isEmpty() && footer.value(u"type"_q).toString() != u"button"_q) {
			throw std::runtime_error("A dialog footer action must be a button.");
		}
		const auto action = std::make_shared<object_ptr<Ui::RpWidget>>(
			footer.isEmpty() ? object_ptr<Ui::RpWidget>(nullptr) : build(*object, nullptr, footer));
		auto box = Box([=](not_null<Ui::GenericBox*> box) {
			box->setTitle(rpl::single(title));
			if (width > 0) {
				const auto available = window->widget()->width() - style::ConvertScale(32);
				box->setWidth(std::min(style::ConvertScale(std::clamp(width, 320, 960)),
					std::max(available, 1)));
			}
			box->addTopButton(st::settingsPluginClose, [=] { box->closeBox(); });
			const auto raw = content->data();
			raw->setParent(box);
			box->addRow(std::move(*content), style::al_justify);
			raw->show();
			PluginUiTree::AddDialogButtons(box, std::move(*action));
			if (const auto i = _objects.find(objectId); i != end(_objects)) {
				i->second->box = box.get();
			}
			connect(box.get(), &QObject::destroyed, this, [=] {
				destroyed(objectId);
			});
		});
		_objects.emplace(objectId, std::move(owned));
		window->uiShow()->showBox(std::move(box));
	} else if (kind == u"window"_q) {
		object->panel = std::make_unique<Ui::SeparatePanel>();
		const auto panel = object->panel.get();
		panel->setWindowFlag(Qt::WindowStaysOnTopHint, false);
		panel->setTitle(rpl::single(title));
		panel->setInnerSize(QSize(
			std::clamp(command.value(u"width"_q).toInt(420), 200, 2000),
			std::clamp(command.value(u"height"_q).toInt(480), 120, 2000)),
			true);
		// Content taller than the window scrolls. ScrollArea isn't an
		// RpWidget, so it fills a plain container that the panel shows.
		auto inner = base::make_unique_q<Ui::RpWidget>(panel);
		const auto scroll = Ui::CreateChild<Ui::ScrollArea>(
			inner.get(),
			st::boxScroll);
		inner->sizeValue() | rpl::on_next([=](QSize size) {
			scroll->setGeometry(QRect(QPoint(), size));
		}, inner->lifetime());
		const auto layout = scroll->setOwnedWidget(
			object_ptr<Ui::VerticalLayout>(scroll));
		auto content = build(*object, layout.data(), tree);
		if (const auto cap = command.value(u"max_content_width"_q).toInt(); cap > 0) {
			content = PluginUiTree::CenterContent(layout, std::move(content), style::ConvertScale(8), cap);
		}
		layout->add(std::move(content), st::boxRowPadding, style::al_justify);
		scroll->widthValue() | rpl::on_next([=](int width) {
			if (layout) {
				layout->resizeToWidth(width);
			}
		}, layout->lifetime());
		panel->showInner(std::move(inner));
		panel->closeRequests(
		) | rpl::on_next([=] {
			// The panel can't be destroyed from its own signal.
			crl::on_main(this, [=] { close(objectId, true); });
		}, panel->lifetime());
		_objects.emplace(objectId, std::move(owned));
		panel->showAndActivate();
	} else if (kind == u"insert"_q) {
		const auto target = Widgets::Resolve(command.value(u"target"_q));
		const auto position = command.value(u"position"_q).toString();
		const auto inside = (position == u"inside"_q);
		if (Core::App().passcodeLocked() || !target || (!inside && position != u"before"_q && position != u"after"_q)) {
			throw std::runtime_error("Invalid insertion target or position.");
		}
		const auto parent = inside ? target : target->parentWidget();
		const auto layout = dynamic_cast<Ui::VerticalLayout*>(parent);
		const auto box = parent ? qobject_cast<QBoxLayout*>(parent->layout()) : nullptr;
		if ((!layout && !box) || ProtectedFromPlugins(parent)
			|| CouldCoverProtected(parent, u"geometry"_q) || InstallationUiActive()) {
			throw std::runtime_error("Target does not accept insertion.");
		}
		const auto ownerRows = std::count_if(_objects.begin(), _objects.end(), [&](const auto &entry) {
			return entry.second->inserted && entry.second->installationId == object->installationId;
		});
		if (ownerRows >= 128) throw std::runtime_error("Too many mounted rows for this installation.");
		const auto count = layout ? layout->count() : box->count();
		auto index = inside ? count : 0;
		if (!inside) {
			if (layout) {
				while (index < count && layout->widgetAt(index).get() != target) ++index;
			} else {
				index = box->indexOf(target);
			}
			if (index < 0 || index == count) throw std::runtime_error("Target is not a layout row.");
			if (position == u"after"_q) ++index;
		}
		auto content = build(*object, parent, tree);
		const auto row = content.data();
		row->setProperty("_ovygram_inserted", true);
		if (layout) {
			layout->insert(index, std::move(content));
		} else {
			box->insertWidget(index, content.release());
		}
		object->inserted = row;
		connect(row, &QObject::destroyed, this, [=] { destroyed(objectId); });
		_objects.emplace(objectId, std::move(owned));
		row->show();
	} else if (kind == u"overlay"_q) {
		const auto target = Widgets::Resolve(command.value(u"target"_q));
		if (!target) {
			throw std::runtime_error("The overlay target no longer exists.");
		} else if (tree.value(u"type"_q).toString() != u"canvas"_q) {
			throw std::runtime_error("An overlay must be a single Canvas.");
		}
		const auto clickable = command.value(u"clickable"_q).toBool();
		auto canvasTree = tree;
		canvasTree.insert(u"height"_q, target->height());
		auto canvas = build(*object, target, canvasTree);
		canvas->setAttribute(Qt::WA_TransparentForMouseEvents, !clickable);
		canvas->setGeometry(target->rect());
		canvas->show();
		canvas->raise();
		new OverlayWatcher(target, canvas.data());
		object->overlay = base::unique_qptr<Ui::RpWidget>(canvas.release());
		connect(object->overlay.get(), &QObject::destroyed, this, [=] {
			destroyed(objectId);
		});
		_objects.emplace(objectId, std::move(owned));
	} else if (kind == u"chat_bar"_q) {
		object->accountId = command.value(u"account_id"_q).toString();
		object->chatId = command.value(u"chat_id"_q).toString();
		if (object->accountId.isEmpty() || object->chatId.isEmpty()) {
			throw std::runtime_error("A chat bar needs account_id and chat_id.");
		}
		object->bar = base::unique_qptr<Ui::RpWidget>(
			new BarFrame(build(*object, nullptr, tree)));
		object->bar->hide();
		connect(object->bar.get(), &QObject::destroyed, this, [=] {
			destroyed(objectId);
		});
		_objects.emplace(objectId, std::move(owned));
		relayoutChats();
	} else if (kind == u"section"_q) {
		const auto window = Core::App().activePrimaryWindow();
		const auto controller = window ? window->sessionController() : nullptr;
		if (!controller) {
			throw std::runtime_error("No Telegram window with an account.");
		}
		// Owned here and shown by the section widget, which the window
		// creates and destroys as the user navigates.
		auto layout = base::make_unique_q<Ui::VerticalLayout>(
			static_cast<QWidget*>(nullptr));
		auto content = build(*object, layout.get(), tree);
		if (const auto cap = command.value(u"max_content_width"_q).toInt(); cap > 0) {
			content = PluginUiTree::CenterContent(layout.get(), std::move(content), 0, cap);
		}
		layout->add(std::move(content), st::boxRowPadding, style::al_justify);
		layout->hide();
		object->title = title;
		object->section = base::unique_qptr<Ui::RpWidget>(layout.release());
		connect(object->section.get(), &QObject::destroyed, this, [=] {
			destroyed(objectId);
		});
		_objects.emplace(objectId, std::move(owned));
		ShowPluginSection(controller, objectId);
	} else {
		throw std::runtime_error("Unknown surface kind.");
	}
}

int PluginUi::layoutChatBars(
		QWidget *container,
		PeerData *peer,
		int left,
		int top,
		int width) {
	if (!container) {
		return 0;
	}
	const auto known = std::find_if(begin(_chats), end(_chats), [&](
			const QPointer<QWidget> &chat) {
		return chat.data() == container;
	});
	if (known == end(_chats)) {
		std::erase_if(_chats, [](const QPointer<QWidget> &chat) {
			return !chat;
		});
		_chats.emplace_back(container);
		// A bar lives until the plugin closes it: take it out of a chat
		// that is destroyed. QWidget emits destroyed() before it deletes
		// its children.
		connect(container, &QObject::destroyed, this, [=] {
			for (const auto &[id, object] : _objects) {
				const auto bar = object->bar.get();
				if (bar && bar->parentWidget() == container) {
					bar->hide();
					bar->setParent(nullptr);
				}
			}
		});
	}
	const auto accountId = peer
		? QString::number(peer->session().uniqueId())
		: QString();
	const auto chatId = peer ? QString::number(peer->id.value) : QString();
	auto height = 0;
	for (const auto &[id, object] : _objects) {
		const auto bar = object->bar.get();
		if (!bar) {
			continue;
		} else if (peer
			&& object->accountId == accountId
			&& object->chatId == chatId) {
			if (bar->parentWidget() != container) {
				// On top of the history, but under the panels and menus
				// the chat raises later.
				bar->setParent(container);
				bar->raise();
			}
			bar->resizeToWidth(width);
			bar->moveToLeft(left, top + height, container->width());
			bar->show();
			height += bar->height();
		} else if (bar->parentWidget() == container) {
			bar->hide();
		}
	}
	return height;
}

void PluginUi::relayoutChats() {
	// Deferred: a command can arrive while the chat is still switching
	// peers, and the layout must see the peer it ends up with.
	if (_relayoutQueued) {
		return;
	}
	_relayoutQueued = true;
	crl::on_main(this, [=] {
		_relayoutQueued = false;
		relayoutChatsNow();
	});
}

void PluginUi::relayoutChatsNow() {
	std::erase_if(_chats, [](const QPointer<QWidget> &chat) {
		return !chat;
	});
	for (const auto &chat : _chats) {
		// The chat recomputes its layout, and LayoutChatBars, on resize.
		auto event = QResizeEvent(chat->size(), chat->size());
		QCoreApplication::sendEvent(chat.data(), &event);
	}
}

object_ptr<Ui::RpWidget> PluginUi::build(
		Object &object,
		QWidget *parent,
		const QJsonObject &node) {
	const auto objectId = object.objectId;
	// A dialog belongs to the window and may outlive the dispatcher.
	const auto guard = QPointer<PluginUi>(this);
	return object.tree.build(parent, node,
		[=, this](QString id, QString kind, QJsonValue value) {
			if (!guard || _updating || InstallationUiActive() || Core::App().passcodeLocked()) {
				return;
			}
			if (const auto i = _objects.find(objectId);
					i != end(_objects) && !i->second->silent) {
				send(*i->second, id, kind, std::move(value));
			}
		});
}

void PluginUi::update(const QJsonObject &command) {
	const auto i = _objects.find(command.value(u"object_id"_q).toString());
	if (i == end(_objects)) {
		return;
	}
	auto &object = *i->second;
	const auto was = std::exchange(_updating, true);
	const auto restore = gsl::finally([&] { _updating = was; });
	object.tree.update(command.value(u"node_id"_q).toString(),
		command.value(u"props"_q).toObject(), !object.overlay);
	if (object.bar) {
		relayoutChats();
	}
}

void PluginUi::close(const QString &objectId, bool notify) {
	const auto i = _objects.find(objectId);
	if (i == end(_objects)) {
		return;
	}
	const auto object = i->second.get();
	object->silent = object->silent || !notify;
	if (object->box) {
		object->box->closeBox(); // destroyed() erases it later.
		return;
	}
	if (!object->silent) {
		send(*object, QString(), u"closed"_q);
	}
	// Deleted after erase(): their destroyed() handler looks up _objects.
	auto inserted = object->inserted;
	auto bar = std::move(object->bar);
	auto overlay = std::move(object->overlay);
	auto section = std::move(object->section);
	_objects.erase(i);
	delete inserted.data();
	overlay = nullptr;
	if (section) {
		// Leaves the window history and the screen.
		_sectionClosed.fire_copy(objectId);
		section = nullptr;
	}
	if (bar) {
		bar = nullptr;
		relayoutChats();
	}
}

void PluginUi::closePlugin(const QString &installationId) {
	auto ids = std::vector<QString>();
	for (const auto &[id, object] : _objects) {
		if (object->installationId == installationId) {
			ids.push_back(id);
		}
	}
	for (const auto &id : ids) {
		close(id, false);
	}
}

void PluginUi::destroyed(const QString &objectId) {
	const auto i = _objects.find(objectId);
	if (i == end(_objects)) {
		return;
	}
	if (!i->second->silent) {
		send(*i->second, QString(), u"closed"_q);
	}
	i->second->silent = true;
	// QWidget emits destroyed() from its own destructor, before QPointer is
	// cleared, so the bar must be released here, not deleted a second time.
	[[maybe_unused]] const auto released = i->second->bar.release();
	[[maybe_unused]] const auto overlay = i->second->overlay.release();
	const auto section = i->second->section.release();
	_objects.erase(i);
	if (section) {
		_sectionClosed.fire_copy(objectId);
	}
}

Ui::RpWidget *PluginUi::sectionContent(const QString &objectId) const {
	const auto i = _objects.find(objectId);
	return (i != end(_objects)) ? i->second->section.get() : nullptr;
}

QString PluginUi::sectionTitle(const QString &objectId) const {
	const auto i = _objects.find(objectId);
	return (i != end(_objects)) ? i->second->title : QString();
}

rpl::producer<> PluginUi::sectionClosed(const QString &objectId) const {
	return _sectionClosed.events(
	) | rpl::filter([=](const QString &id) {
		return id == objectId;
	}) | rpl::to_empty;
}

void PluginUi::sectionGone(const QString &objectId) {
	// Deferred: called while the window changes its history.
	crl::on_main(this, [=] {
		close(objectId, true);
	});
}

void PluginUi::send(
		const Object &object,
		const QString &nodeId,
		const QString &type,
		QJsonValue value) {
	auto event = QJsonObject{
		{ u"installation_id"_q, object.installationId },
		{ u"object_id"_q, object.objectId },
		{ u"node_id"_q, nodeId },
		{ u"type"_q, type },
	};
	if (!value.isNull() && !value.isUndefined()) {
		event.insert(u"value"_q, value);
	}
	_send(std::move(event));
}

} // namespace Ovygram
