#include "ui/pie_layout.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::ui::pie {

namespace {

constexpr float kPi = 3.14159265358979f;

bool knobLeft(Shape s) { return s == Shape::LeftUpper || s == Shape::LeftSide || s == Shape::LeftLower; }
bool knobRight(Shape s) { return s == Shape::RightLower || s == Shape::RightSide || s == Shape::RightUpper; }

int itemWidth(Shape shape, int textWidth) {
    if (shape == Shape::Below || shape == Shape::Above) {
        const int half = std::max(0, (textWidth - kStalkWidth) / 2);
        return 2 * kCapWidth + kStalkWidth + 2 * half;
    }
    if (shape == Shape::Centre) {
        return textWidth + 2 * kCapWidth;
    }
    return textWidth + kCapWidth + kKnobCapWidth;
}

// The item's top left from its knob's centre.
std::pair<int, int> itemCorner(Shape shape, int knobX, int knobY, int w) {
    switch (shape) {
    case Shape::Below:      return {knobX - w / 2, knobY - 48};
    case Shape::Above:      return {knobX - w / 2, knobY - 21};
    case Shape::LeftUpper:  return {knobX - 14, knobY - 26};
    case Shape::LeftSide:   return {knobX - 14, knobY - 21};
    case Shape::LeftLower:  return {knobX - 14, knobY - 16};
    case Shape::RightLower: return {knobX + 14 - w, knobY - 16};
    case Shape::RightSide:  return {knobX + 14 - w, knobY - 21};
    case Shape::RightUpper: return {knobX + 14 - w, knobY - 26};
    case Shape::Centre:     break;
    }
    return {knobX - w / 2, knobY - kPillHeight / 2};
}

Item place(Shape shape, int knobX, int knobY, int textWidth, int textHeight) {
    Item item;
    item.shape = shape;
    item.knobX = knobX;
    item.knobY = knobY;
    const int w = itemWidth(shape, textWidth);
    const auto [x, y] = itemCorner(shape, knobX, knobY, w);
    const bool stalk = shape == Shape::Below || shape == Shape::Above;
    item.rect = {x, y, w, stalk ? kStalkHeight : kPillHeight};
    item.textX = knobLeft(shape) ? kKnobCapWidth : knobRight(shape) || shape == Shape::Centre ? kCapWidth : (w - textWidth) / 2;
    item.textY = (kPillHeight - textHeight) / 2 + (shape == Shape::Above ? kStalkHeight - kPillHeight : 0);
    return item;
}

void grow(Rect& r, const Rect& by) {
    const int x1 = std::max(r.x + r.w, by.x + by.w), y1 = std::max(r.y + r.h, by.y + by.h);
    r.x = std::min(r.x, by.x);
    r.y = std::min(r.y, by.y);
    r.w = x1 - r.x;
    r.h = y1 - r.y;
}

} // namespace

Shape shapeAt(float angle) {
    if (angle == 0) {
        return Shape::Below;
    }
    if (angle > 11 * kPi / 12 && angle < 13 * kPi / 12) {
        return Shape::Above;
    }
    if (angle < kPi / 4) return Shape::LeftUpper;
    if (angle <= 3 * kPi / 4) return Shape::LeftSide;
    if (angle < kPi) return Shape::LeftLower;
    if (angle < 5 * kPi / 4) return Shape::RightLower;
    if (angle <= 7 * kPi / 4) return Shape::RightSide;
    return Shape::RightUpper;
}

Layout layout(int centreX, int centreY, std::span<const int> textWidths, int textHeight, int headWidth, int headHeight,
              int screenWidth, int screenHeight) {
    Layout menu;
    menu.centreX = centreX;
    menu.centreY = centreY;
    menu.bounds = {centreX - headWidth / 2, centreY - headHeight / 2, headWidth, headHeight};
    const float step = static_cast<float>(2 * 3.14159265358979 / static_cast<double>(std::max<std::size_t>(textWidths.size(), 1)));
    for (std::size_t i = 0; i < textWidths.size(); ++i) {
        const float angle = static_cast<float>(i) * step;
        const int knobX = centreX + static_cast<int>(kItemRadius * std::sin(angle));
        const int knobY = centreY - static_cast<int>(kItemRadius * std::cos(angle));
        menu.items.push_back(place(shapeAt(angle), knobX, knobY, textWidths[i], textHeight));
        grow(menu.bounds, menu.items.back().rect);
    }
    // Back onto the screen, everything together.
    const Rect& b = menu.bounds;
    int dx = 0, dy = 0;
    if (b.x + b.w > screenWidth - 1) dx = screenWidth - 1 - (b.x + b.w);
    if (b.x + dx < 1) dx = 1 - b.x;
    if (b.y + b.h > screenHeight - 1) dy = screenHeight - 1 - (b.y + b.h);
    if (b.y + dy < 1) dy = 1 - b.y;
    menu.centreX += dx;
    menu.centreY += dy;
    menu.bounds.x += dx;
    menu.bounds.y += dy;
    for (auto& item : menu.items) {
        item.knobX += dx;
        item.knobY += dy;
        item.rect.x += dx;
        item.rect.y += dy;
    }
    return menu;
}

Item centreItem(int x, int y, int textWidth, int textHeight) { return place(Shape::Centre, x, y, textWidth, textHeight); }

bool over(const Item& item, int x, int y) {
    const Rect& r = item.rect;
    if (item.shape != Shape::Below && item.shape != Shape::Above) {
        return r.contains(x, y);
    }
    // A stalked item is its pill and its knob, not the corners beside the stalk.
    const int pillY = item.shape == Shape::Above ? r.y + kStalkHeight - kPillHeight : r.y;
    const Rect pill{r.x, pillY, r.w, kPillHeight};
    const Rect knob{r.x + (r.w - kStalkWidth) / 2, r.y, kStalkWidth, kStalkHeight};
    return pill.contains(x, y) || knob.contains(x, y);
}

std::optional<std::size_t> pick(const Layout& menu, int x, int y) {
    const Rect& b = menu.bounds;
    const Rect reach{b.x - kCancelBoundary, b.y - kCancelBoundary, b.w + 2 * kCancelBoundary, b.h + 2 * kCancelBoundary};
    const int dx = x - menu.centreX, dy = y - menu.centreY;
    if (!reach.contains(x, y) || dx * dx + dy * dy <= kGesturePickBoundary * kGesturePickBoundary) {
        return std::nullopt;
    }
    // The first item under the mouse, or else the one whose direction from the centre is nearest the
    // mouse's, as the largest dot product. A direction more than a right angle off picks nothing.
    std::optional<std::size_t> best;
    int bestDot = 0;
    for (std::size_t i = 0; i < menu.items.size(); ++i) {
        const auto& item = menu.items[i];
        if (over(item, x, y)) {
            return i;
        }
        const int dot = (item.knobX - menu.centreX) * dx + (item.knobY - menu.centreY) * dy;
        if (dot > bestDot) {
            bestDot = dot;
            best = i;
        }
    }
    return best;
}

} // namespace ts2::ui::pie
