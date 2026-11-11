#pragma once

#include "formats/tga.hpp"
#include "sim/world.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace app {

// The game's text notifications: the message bubbles stacked at the top right of live mode. The
// newest goes on top, pushing the others down, and slides in from the right. Each closes when its
// X is clicked or its timeout runs out (seconds of real time, stopped while the game is paused). A
// change of text, picture or timing starts its countdown again and sorts the stack by priority.
class NotificationStack {
public:
    // What a bubble's pictures and tips show. Pictures may be null while they load. A portrait of
    // side 0 is the whole portrait at its own size, any other side its middle at that size.
    struct Look {
        std::function<const ts2::tga::Pixels*(std::int16_t neighbor, int side)> portrait;
        std::function<const ts2::tga::Pixels*(std::uint32_t objectType, int side)> objectPicture;
        std::function<std::string(std::int16_t neighbor)> neighborName;
        std::function<std::string(std::uint32_t objectType)> objectName;
        // The neighbor of a Sim on the lot (0 for an object), and an object's type (0 when gone).
        std::function<std::int16_t(std::int16_t object)> neighborOf;
        std::function<std::uint32_t(std::int16_t object)> typeOf;
    };
    // Brings the bubbles in line with the simulation's notifications. Returns those whose timeout
    // ran out, which the caller removes from the simulation.
    std::vector<std::int16_t> update(ts2::ui::Screen& screen, const std::vector<ts2::sim::Notification>& shown, double realSeconds,
                                     bool paused, const Look& look);
    // A click on one of the bubbles' buttons: the X (or Ignore) gives the notification to close, a
    // Sim's picture the neighbor whose Sim the camera should centre on.
    struct Click {
        std::optional<std::int16_t> close;
        std::optional<std::int16_t> neighbor;
    };
    std::optional<Click> clicked(ts2::ui::Screen& screen, int window) const;
    // Takes every bubble off the screen.
    void clear(ts2::ui::Screen& screen);

private:
    struct Move {
        int dx = 0, dy = 0;
        double seconds = 0;
    };
    struct Bubble {
        std::int16_t id = 0;
        std::uint32_t version = 0;
        std::uint8_t priority = 0, timeout = 0;
        int root = -1;
        std::int16_t neighbor = 0;  // whose Sim its picture shows, 0 for none
        bool waiting = false;       // a picture it shows was still loading
        double queuedAt = 0;        // real seconds, when it first waited to slide in
        double shownAt = 0;         // on the stack's clock, when it last joined or changed
        // Where its moves start from, and the moves left.
        double fromX = 0, fromY = 0;
        std::vector<Move> moves;
        double moveStarted = 0;
    };
    int load(ts2::ui::Screen& screen, std::uint8_t style);
    void fill(ts2::ui::Screen& screen, Bubble& b, const ts2::sim::Notification& n, const Look& look);
    static bool extraDiffers(const ts2::sim::Notification& n, const Look& look);
    static bool picturesReady(const ts2::sim::Notification& n, const Look& look);
    void layOut(ts2::ui::Screen& screen);
    void animate(ts2::ui::Screen& screen);
    void startMoves(Bubble& b, std::vector<Move> moves);
    bool moving() const;
    std::vector<Bubble> shown_;    // top first
    std::vector<Bubble> pending_;  // highest priority first
    std::optional<Bubble> sliding_;
    int state_ = 0;      // 0 idle, 1 pushing the stack down, 2 sliding a new one in
    double clock_ = 0;   // seconds the game wasn't paused
    double now_ = 0;     // real seconds, for the moves
    int screenWidth_ = 0;
    struct Constants {
        int topOffset = 8, rightOffset = 10, margin = 3;
    };
    std::optional<Constants> constants_;
};

} // namespace app
