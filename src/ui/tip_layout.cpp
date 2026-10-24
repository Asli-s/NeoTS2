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
    const int left = limited ? kInset : INT_MIN / 4, top = limited ? kInset : INT_MIN / 4;
    const int right = limited ? screen[0] - kInset : INT_MAX / 4, bottom = limited ? screen[1] - kInset : INT_MAX / 4;
    const int roomLeft = ax - left, roomRight = right - ax, roomUp = ay - top, roomDown = bottom - ay;

    enum class Side { Left, Above, Right, Below };
    Side side;
    if (roomLeft < kCell && roomUp > kCell && roomDown > kCell) {
        side = Side::Right;
    } else if (roomRight < kCell && roomUp > kCell && roomDown > kCell) {
        side = Side::Left;
    } else if (roomUp > h + kGap) {
        side = Side::Above;
    } else if (roomDown > h + kGap) {
        side = Side::Below;
    } else if (roomLeft > w + kGap) {
        side = Side::Left;
    } else {
        side = roomRight > w + kGap ? Side::Right : Side::Above;
    }

    TipBox box;
    if (side == Side::Above || side == Side::Below) {
        box.w = std::max(w, 3 * kCell);
        box.h = std::max(h, 3 * kCell / 2);
        box.x = clampLowFirst(ax - box.w / 2, left, right - box.w);
        box.y = side == Side::Above ? ay - kGap - box.h : ay + kGap;
    } else {
        box.w = w;
        box.h = std::max(h, 2 * kCell);
        box.x = side == Side::Left ? ax - kGap - box.w : ax + kGap;
        box.y = clampLowFirst(ay - box.h / 2, top, bottom - box.h);
    }
    return box;
}

} // namespace ts2::ui
