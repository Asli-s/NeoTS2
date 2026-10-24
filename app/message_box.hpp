#pragma once

#include "platform/platform.hpp"

#include <string>

namespace app {

using MessageKind = ts2::platform::MessageKind;

// A message for the player, in UTF-8. On Windows it is a task dialog of a fixed width with room
// around the text, as the launcher shows its messages. Elsewhere it is the platform's message box.
void showMessage(MessageKind kind, const std::string& text, ts2::platform::Window* parent = nullptr);

} // namespace app
