#pragma once

#include <string>

struct SDL_Window;

namespace app {

enum class MessageKind { Error, Warning, Information };

// A message for the player, in UTF-8. On Windows it is a task dialog of a fixed width with room
// around the text, as the launcher shows its messages. Elsewhere it is SDL's message box.
void showMessage(MessageKind kind, const std::string& text, SDL_Window* parent = nullptr);

} // namespace app
