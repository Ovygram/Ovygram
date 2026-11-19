#pragma once

class PeerData;
class QWidget;

namespace Ui {
class InputField;
} // namespace Ui

namespace Ovygram::Commands {

// Commands of plugins in the message field: the list above the field, the
// grey completion after the cursor and running a command instead of sending
// its text. What to show is decided by the manager (ovygram_commands.py);
// this draws it and passes the keys.

// Called by ComposeControls (the only upstream-facing part). wrap is the
// widget the list is placed above; peer returns the chat of the field.
void Attach(
	not_null<QWidget*> wrap,
	not_null<Ui::InputField*> field,
	Fn<PeerData*()> peer);

// Whether the text of the field is a plugin command. Then it has run or was
// refused with an explanation, and must not be sent.
[[nodiscard]] bool Submit(not_null<Ui::InputField*> field);

// Called by PluginClient when suggestions of a plugin have arrived.
void Refresh();

} // namespace Ovygram::Commands
