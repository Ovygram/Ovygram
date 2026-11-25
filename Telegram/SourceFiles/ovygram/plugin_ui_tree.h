#pragma once

#include "base/basic_types.h"
#include "base/object_ptr.h"

#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <rpl/variable.h>

#include <map>
#include <memory>

namespace Ui {
class GenericBox;
class RpWidget;
} // namespace Ui

namespace Ovygram {

[[nodiscard]] QJsonObject PluginStylePalette();

class PluginUiTree final {
public:
	using Event = Fn<void(QString nodeId, QString type, QJsonValue value)>;

	[[nodiscard]] object_ptr<Ui::RpWidget> build(
		QWidget *parent,
		const QJsonObject &node,
		const Event &event,
		int depth = 0);
	void update(const QString &nodeId, const QJsonObject &props, bool relayoutParent = true);
	static void AddDialogButtons(
		not_null<Ui::GenericBox*> box,
		object_ptr<Ui::RpWidget> footerAction);
	[[nodiscard]] static object_ptr<Ui::RpWidget> CenterContent(
		QWidget *parent,
		object_ptr<Ui::RpWidget> content,
		int padding,
		int cap);
	static void ResizeCanvas(not_null<Ui::RpWidget*> widget, QSize size);

private:
	std::map<QString, QPointer<Ui::RpWidget>> _nodes;
	std::map<QString, std::shared_ptr<rpl::variable<QString>>> _texts;
	int _count = 0;
};

} // namespace Ovygram
