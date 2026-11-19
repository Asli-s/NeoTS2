#include "ui/tip_layout.hpp"

#include <algorithm>
#include <climits>
#include <optional>
#include <vector>

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

std::optional<std::uint32_t> hexNumber(std::string_view s) {
    std::size_t at = 0;
    while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r')) {
        ++at;
    }
    if (at + 1 < s.size() && s[at] == '0' && (s[at + 1] == 'x' || s[at + 1] == 'X')) {
        at += 2;
    }
    std::uint32_t value = 0;
    bool digits = false;
    for (; at < s.size(); ++at) {
        const char c = s[at];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) {
            break;
        }
        value = value * 16 + static_cast<std::uint32_t>(d);
        digits = true;
    }
    return digits ? std::optional(value) : std::nullopt;
}

TipText splitTip(std::string_view text) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t bar = text.find('|', start);
        fields.push_back(text.substr(start, bar == std::string_view::npos ? std::string_view::npos : bar - start));
        if (bar == std::string_view::npos) {
            break;
        }
        start = bar + 1;
    }
    const auto field = [&](std::size_t i) { return i < fields.size() ? fields[i] : std::string_view(); };
    const bool withExtra = std::count(text.begin(), text.end(), '|') == 5;
    TipText tip;
    tip.title = field(0);
    tip.body = field(1);
    std::size_t next = 2;
    if (withExtra) {
        tip.extra = field(next++);
    }
    tip.picture1 = hexNumber(field(next++)).value_or(0);
    tip.picture2 = hexNumber(field(next++)).value_or(0);
    const std::string_view mode = field(next);
    if (mode == "L") {
        tip.mode = TipText::Mode::Large;
    } else if (const auto script = hexNumber(mode)) {
        tip.mode = TipText::Mode::Popup;
        tip.popup = *script;
    }
    return tip;
}

} // namespace ts2::ui
