#pragma once

#include "base/basic_types.h"
#include "ovygram/auto_update_verify.h"

#include <QtCore/QString>

#include <optional>

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Ovygram {

struct UpdateChannelBoxData {
	UpdateChannel currentChannel = UpdateChannel::Stable;
	QString currentVersionLabel;
	std::optional<UpdateRelease> readyRelease;
};

void BuildUpdateChannelBox(
	not_null<Ui::GenericBox*> box,
	UpdateChannelBoxData data,
	Fn<void(UpdateChannel)> onSave);

void BuildUpdateReadyBox(
	not_null<Ui::GenericBox*> box,
	const UpdateRelease &release,
	Fn<void()> onRestartAndUpdate);

} // namespace Ovygram
