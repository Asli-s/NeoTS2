#pragma once

#include <cstdint>

namespace app {

// Drops mouse clicks while a scene loads, as the game's loading screens take none. A button pressed
// while loading has its release dropped too, even after loading ends, so nothing sees half a click.
// A button pressed before loading keeps its release.
class ClickGate {
public:
    // Whether a press (`down`) or release of mouse button `button` is dropped. Buttons past 7 are
    // never dropped.
    bool drops(bool down, unsigned button, bool loading) {
        if (button >= 8) {
            return false;
        }
        const auto bit = static_cast<std::uint8_t>(1u << button);
        if (down && loading) {
            held_ |= bit;
            return true;
        }
        if (!down && (held_ & bit) != 0) {
            held_ &= static_cast<std::uint8_t>(~bit);
            return true;
        }
        return false;
    }

private:
    std::uint8_t held_ = 0;  // the buttons pressed while loading, one bit each
};

} // namespace app
