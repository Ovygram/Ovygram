#include "ovygram/protected_ui.h"

#include <QtCore/QPointer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>
#include <rpl/event_stream.h>

#include <vector>

namespace Ovygram {
namespace {

struct State {
	std::vector<QPointer<QWidget>> surfaces;
	rpl::event_stream<> changes;
};

State &Get() {
	// Leaked on purpose: surfaces are destroyed during the shutdown too.
	static const auto result = new State();
	return *result;
}

void Changed() {
	// A surface reports its destruction from its destructor. Observers
	// repaint and show widgets, so they run after it has finished.
	QMetaObject::invokeMethod(qApp, [] {
		auto &state = Get();
		std::erase_if(state.surfaces, [](const QPointer<QWidget> &surface) {
			return !surface;
		});
		state.changes.fire({});
	}, Qt::QueuedConnection);
}

} // namespace

void ProtectSurface(not_null<QWidget*> surface) {
	Get().surfaces.emplace_back(surface.get());
	QObject::connect(surface, &QObject::destroyed, qApp, [] { Changed(); });
	Changed();
}

bool ProtectedFromPlugins(const QObject *object) {
	const auto &surfaces = Get().surfaces;
	if (surfaces.empty() || !object || !object->isWidgetType()) {
		return false;
	}
	const auto widget = static_cast<const QWidget*>(object);
	for (const auto &surface : surfaces) {
		if (surface && (surface == widget
			|| surface->isAncestorOf(widget)
			|| widget->isAncestorOf(surface))) {
			return true;
		}
	}
	return false;
}

bool InsideProtected(const QObject *object) {
	const auto &surfaces = Get().surfaces;
	if (surfaces.empty() || !object || !object->isWidgetType()) {
		return false;
	}
	const auto widget = static_cast<const QWidget*>(object);
	for (const auto &surface : surfaces) {
		if (surface && (surface == widget || surface->isAncestorOf(widget))) {
			return true;
		}
	}
	return false;
}

bool CouldCoverProtected(const QWidget *widget, const QString &member) {
	static const auto kMembers = std::vector<QString>{
		u"geometry"_q, u"pos"_q, u"size"_q, u"visible"_q,
		u"minimumSize"_q, u"maximumSize"_q, u"minimumWidth"_q,
		u"minimumHeight"_q, u"maximumWidth"_q, u"maximumHeight"_q,
		u"windowOpacity"_q, u"raise"_q, u"lower"_q, u"show"_q,
		u"showNormal"_q, u"showMaximized"_q, u"showFullScreen"_q,
		u"showMinimized"_q, u"setVisible"_q, u"setHidden"_q, u"hide"_q,
		u"close"_q, u"setFocus"_q,
	};
	if (std::find(begin(kMembers), end(kMembers), member) == end(kMembers)) {
		return false;
	}
	for (const auto &surface : Get().surfaces) {
		if (surface && surface->window() == widget->window()) {
			return true;
		}
	}
	return false;
}

bool ProtectedSurfaceExists() {
	for (const auto &surface : Get().surfaces) {
		if (surface) {
			return true;
		}
	}
	return false;
}

rpl::producer<> ProtectedSurfacesChanged() {
	return Get().changes.events();
}

} // namespace Ovygram
