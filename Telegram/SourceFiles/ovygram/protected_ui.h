#pragma once

#include <rpl/producer.h>

class QObject;
class QString;
class QWidget;

namespace Ovygram {

// Surfaces of Ovygram itself that plugins must not inspect, change, hide or
// cover: the installer, the plugin manager and their confirmations. The rest
// of Telegram stays open to the Plugin API.
//
// Plugin code runs in another process, so the Plugin API commands checked
// here are its only way to the widgets of the application.

// Protects the widget until it is destroyed.
void ProtectSurface(not_null<QWidget*> surface);

// The widget is a protected surface, lies inside one, or contains one and
// so could hide or move it. Such widgets get no handle and match no rule.
[[nodiscard]] bool ProtectedFromPlugins(const QObject *object);

// The narrower case: the widget is a protected surface or lies inside one.
// A widget tree lists the containers of a surface, never its contents.
[[nodiscard]] bool InsideProtected(const QObject *object);

// A property or method that moves, shows, hides or restacks a widget in a
// window that currently shows a protected surface.
[[nodiscard]] bool CouldCoverProtected(const QWidget *widget, const QString &member);

[[nodiscard]] bool ProtectedSurfaceExists();

// Fires after a surface was protected and after one was destroyed.
[[nodiscard]] rpl::producer<> ProtectedSurfacesChanged();

} // namespace Ovygram
