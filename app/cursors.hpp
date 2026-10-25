#pragma once

#include "platform/platform.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <vector>

namespace ts2::content {
class UiData;
}

namespace app {

// The game's mouse cursors, by the ids the game gives them, from the newest TSData/Res/UI/Cursors
// folder that has each file.
class Cursors {
public:
    // The arrow the game shows over its panels and outside live mode.
    static constexpr std::uint32_t kArrow = 0x01;
    // Live mode: over ground, a Sim, an object the selected Sim can use (also over a lot in the
    // neighborhood), and anything else.
    static constexpr std::uint32_t kLiveNothing = 0x2B, kLivePerson = 0x2C, kLiveObjectAvailable = 0x2D, kLiveObjectUnavailable = 0x2E;

    Cursors(const std::filesystem::path& install, ts2::content::UiData& ui);
    ~Cursors();
    Cursors(const Cursors&) = delete;
    Cursors& operator=(const Cursors&) = delete;

    // Shows a cursor by id (the system arrow for one that could not be loaded).
    void show(std::uint32_t id);
    // Shows the camera drag arrow pointing at `angle` radians (screen direction, y up).
    void showDragArrow(float angle);

private:
    std::map<std::uint32_t, ts2::platform::CursorPtr> cursors_;
    std::uint32_t shown_ = 0;
    std::vector<std::uint8_t> arrow_;  // the drag arrow's grey picture, 32 x 32
    ts2::platform::CursorPtr turned_;  // the drag arrow as last turned
    float turnedAngle_ = 0;
};

} // namespace app
