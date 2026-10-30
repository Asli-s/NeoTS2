#include "ui/tip_layout.hpp"

#include <algorithm>
#include <climits>

namespace ts2::ui {

namespace {

constexpr int kInset = 16;  // the screen's margin the box stays inside
constexpr int kGap = 8;     // between the anchor and the box
constexpr int kCell = 6;    // a sixth of the frame picture's 30 pixels

// Kept within [low, high], the low end winning when the box doesn't fit, as the game clamps.
int clampLowFirst(int v, int low, int high) {
    return v < low ? low : std::min(v, high);
}

} // namespace

TipBox placeTip(int ax, int ay, int w, int h, std::array<int, 2> screen) {
    const bool limited = screen[0] > 0 && screen[1] > 0;
    const std::array<int, 4> bounds = limited ? std::array<int, 4>{kInset, kInset, screen[0] - kInset, screen[1] - kInset}
                                              : std::array<int, 4>{INT_MIN / 4, INT_MIN / 4, INT_MAX / 4, INT_MAX / 4};
    return placeTip(ax, ay, w, h, bounds, kGap);
}

TipBox placeTip(int ax, int ay, int w, int h, std::array<int, 4> bounds, int gap) {
    const auto [left, top, right, bottom] = bounds;
    const int roomLeft = ax - left, roomRight = right - ax, roomUp = ay - top, roomDown = bottom - ay;

    enum class Side { Left, Above, Right, Below };
    Side side;
    if (roomLeft < kCell && roomUp > kCell && roomDown > kCell) {
        side = Side::Right;
    } else if (roomRight < kCell && roomUp > kCell && roomDown > kCell) {
        side = Side::Left;
    } else if (roomUp > h + gap) {
        side = Side::Above;
    } else if (roomDown > h + gap) {
        side = Side::Below;
    } else if (roomLeft > w + gap) {
        side = Side::Left;
    } else {
        side = roomRight > w + gap ? Side::Right : Side::Above;
    }

    TipBox box;
    if (side == Side::Above || side == Side::Below) {
        box.w = std::max(w, 3 * kCell);
        box.h = std::max(h, 3 * kCell / 2);
        box.x = clampLowFirst(ax - box.w / 2, left, right - box.w);
        box.y = side == Side::Above ? ay - gap - box.h : ay + gap;
    } else {
        box.w = w;
        box.h = std::max(h, 2 * kCell);
        box.x = side == Side::Left ? ax - gap - box.w : ax + gap;
        box.y = clampLowFirst(ay - box.h / 2, top, bottom - box.h);
    }
    return box;
}

} // namespace ts2::ui
