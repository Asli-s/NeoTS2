#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace ts2::ui {

// Where the game's pie menu puts its items. Items go round the centre clockwise from the top, each
// a pill with a knob pointing at the centre. The knob's side depends on the item's angle.
namespace pie {

constexpr int kItemRadius = 68;          // centre to an item's knob
constexpr int kCancelBoundary = 30;      // beyond the menu by this much, nothing is picked
constexpr int kGesturePickBoundary = 35; // within this of the centre, nothing is picked
constexpr int kPillHeight = 42;
constexpr int kStalkHeight = 68;         // an item with its knob above or below the pill
constexpr int kStalkWidth = 35;
constexpr int kCapWidth = 16;
constexpr int kKnobCapWidth = 38;

// How an item is drawn: its knob below the pill (top item), above it (bottom item), at the pill's
// left end (right half, from top to bottom: upper, side, lower) or its right end (left half, from
// bottom to top: lower, side, upper). The centre item a sub-menu goes back with has no knob.
enum class Shape { Centre, Below, LeftUpper, LeftSide, LeftLower, Above, RightLower, RightSide, RightUpper };

Shape shapeAt(float angle);

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

struct Item {
    Shape shape = Shape::Centre;
    int knobX = 0, knobY = 0;  // the knob's centre, whose offset from the menu's centre is the item's direction
    Rect rect;                 // the whole item
    int textX = 0, textY = 0;  // the text's top left, inside the item
};

struct Layout {
    int centreX = 0, centreY = 0;
    std::vector<Item> items;
    Rect bounds;  // the head's area and every item
};

// The items for texts `textWidths` wide and `textHeight` high round (centreX, centreY): the head's
// area (`headWidth` x `headHeight`) centred there. The whole menu is moved back onto a
// `screenWidth` x `screenHeight` screen, 1 pixel from its edges.
Layout layout(int centreX, int centreY, std::span<const int> textWidths, int textHeight, int headWidth, int headHeight,
              int screenWidth, int screenHeight);

// The centre item a sub-menu shows to go back, centred on (x, y).
Item centreItem(int x, int y, int textWidth, int textHeight);

// Whether (x, y) is on the item's pictures: its pill and its knob.
[[nodiscard]] bool over(const Item& item, int x, int y);

// The item the mouse at (x, y) picks. None beyond the menu or near its centre. Otherwise the first
// one under the mouse, or else the one whose direction from the centre is nearest the mouse's, if
// less than a right angle off.
[[nodiscard]] std::optional<std::size_t> pick(const Layout& menu, int x, int y);

} // namespace pie

} // namespace ts2::ui
