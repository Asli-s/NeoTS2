#pragma once

#include "formats/cursor.hpp"
#include "platform/platform.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
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
    // The game's animated hourglass, shown while a lot or neighborhood loads.
    static constexpr std::uint32_t kHourglass = 0x0F;
    // Live mode: over ground, a Sim, an object the selected Sim can use (also over a lot in the
    // neighborhood), and anything else.
    static constexpr std::uint32_t kLiveNothing = 0x2B, kLivePerson = 0x2C, kLiveObjectAvailable = 0x2D, kLiveObjectUnavailable = 0x2E;

    Cursors(const std::filesystem::path& install, ts2::content::UiData& ui);
    ~Cursors();
    Cursors(const Cursors&) = delete;
    Cursors& operator=(const Cursors&) = delete;

    // Shows a cursor by id (the system arrow for one that could not be loaded). While busy, it is
    // shown once the hourglass goes.
    void show(std::uint32_t id);
    // Shows the camera drag arrow pointing at `angle` radians (screen direction, y up). Not while busy.
    void showDragArrow(float angle);
    // Shows the hourglass over every other cursor while `busy`, then the cursor last asked for.
    void setBusy(bool busy);
    // Moves an animated cursor on to its picture at `seconds` (any steady clock).
    void animate(double seconds);

    // A cursor's pictures and, for an animated one, the steps that show them in turn.
    struct Shape {
        std::vector<ts2::platform::CursorPtr> frames;
        std::vector<ts2::cursor::AnimatedCursor::Step> steps;
    };

private:
    void display(std::uint32_t id);

    std::map<std::uint32_t, Shape> cursors_;
    std::uint32_t shown_ = 0;
    std::uint32_t wanted_ = kArrow;  // the cursor last asked for, shown when not busy
    bool busy_ = false;
    ts2::cursor::CursorPlayback playback_;      // the animated cursor's steps
    const ts2::platform::Cursor* set_ = nullptr;  // the picture last set
    std::vector<std::uint8_t> arrow_;  // the drag arrow's grey picture, 32 x 32
    ts2::platform::CursorPtr turned_;  // the drag arrow as last turned
    float turnedAngle_ = 0;
};

} // namespace app
