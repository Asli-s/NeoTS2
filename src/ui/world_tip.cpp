#include "ui/world_tip.hpp"

#include <algorithm>
#include <cstddef>

namespace ts2::ui {

namespace {

constexpr int kSideMargin = 8, kEndMargin = 3;  // room between the text and the box's sides and its top and bottom
constexpr int kTextLead = 4;                    // more room left of the text
constexpr int kGap = 28;                        // from the spot to the box's near side

// `v` kept between `lo` and `hi`. When they cross, `lo` wins for a value below it.
int keep(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

struct Rect {
    int l, t, r, b;
};

// Copies `src` of the sheet into `out` over `dst`, repeating it from dst's top left and cutting it
// off at dst's edges. Copied pixels replace what was there, so the arrow opens the box's edge.
void tile(const tga::Pixels& sheet, Rect src, tga::Pixels& out, Rect dst) {
    const int sw = src.r - src.l, sh = src.b - src.t;
    if (sw <= 0 || sh <= 0) {
        return;
    }
    const int ow = static_cast<int>(out.width), oh = static_cast<int>(out.height), shw = static_cast<int>(sheet.width);
    for (int y = std::max(dst.t, 0); y < std::min(dst.b, oh); ++y) {
        const int sy = src.t + (y - dst.t) % sh;
        for (int x = std::max(dst.l, 0); x < std::min(dst.r, ow); ++x) {
            const int sx = src.l + (x - dst.l) % sw;
            const auto from = (static_cast<std::size_t>(sy) * static_cast<std::size_t>(shw) + static_cast<std::size_t>(sx)) * 4;
            const auto to = (static_cast<std::size_t>(y) * static_cast<std::size_t>(ow) + static_cast<std::size_t>(x)) * 4;
            std::copy_n(sheet.rgba.begin() + static_cast<std::ptrdiff_t>(from), 4, out.rgba.begin() + static_cast<std::ptrdiff_t>(to));
        }
    }
}

} // namespace

WorldTipLayout layoutWorldTip(int anchorX, int anchorY, int textW, int textH, std::array<int, 4> bounds, int cell) {
    const int c = cell;
    const auto [left, top, right, bottom] = bounds;
    int w = textW + 2 * kSideMargin + kTextLead, h = textH + 2 * kEndMargin;
    // The box goes to the right or left of a spot within a cell of the screen's side, else above
    // when it fits there, else below, else to whichever side it fits.
    const int roomL = anchorX - left, roomR = right - anchorX, roomT = anchorY - top, roomB = bottom - anchorY;
    WorldTipLayout out;
    if (roomL < c && c < roomT && c < roomB) {
        out.side = 2;
    } else if (roomR < c && c < roomT && c < roomB) {
        out.side = 0;
    } else if (h + kGap < roomT) {
        out.side = 1;
    } else if (h + kGap < roomB) {
        out.side = 3;
    } else if (w + kGap < roomL) {
        out.side = 0;
    } else {
        out.side = w + kGap < roomR ? 2 : 1;
    }
    // A box beside its spot is at least 2 cells tall and centred on it. One above or below is at
    // least 3 cells wide and 1.5 tall, centred over it. Either way it stays inside the bounds, and the
    // arrow slides along the box to the spot, keeping a cell from a side box's ends and a cell and a
    // half from the ends of one above or below.
    Rect box{}, win{}, arrow{};
    if (out.side == 0 || out.side == 2) {
        h = std::max(h, 2 * c);
        box.l = out.side == 0 ? anchorX - kGap - w : anchorX + kGap;
        box.r = box.l + w;
        box.t = keep(anchorY - h / 2, top, bottom - h);
        box.b = box.t + h;
        win = box;
        const int at = keep(anchorY, box.t + c, box.b - c);
        arrow.t = at - c / 2;
        arrow.b = arrow.t + c;
        if (out.side == 0) {
            win.r += 3 * c / 2;
            arrow.l = box.r - c / 2;
            arrow.r = arrow.l + 2 * c;
        } else {
            win.l -= 3 * c / 2;
            arrow.r = box.l + c / 2;
            arrow.l = arrow.r - 2 * c;
        }
    } else {
        w = std::max(w, 3 * c);
        h = std::max(h, 3 * c / 2);
        box.l = keep(anchorX - w / 2, left, right - w);
        box.r = box.l + w;
        box.t = out.side == 1 ? anchorY - kGap - h : anchorY + kGap;
        box.b = box.t + h;
        win = box;
        const int at = keep(anchorX, box.l + c / 2 + c, box.r - c / 2 - c);
        arrow.l = at - c / 2;
        arrow.r = arrow.l + c;
        if (out.side == 1) {
            win.b += 3 * c / 2;
            arrow.t = box.b - c / 2;
            arrow.b = arrow.t + 2 * c;
        } else {
            win.t -= 3 * c / 2;
            arrow.b = box.t + c / 2;
            arrow.t = arrow.b - 2 * c;
        }
    }
    out.window = {win.l, win.t, win.r, win.b};
    out.box = {box.l, box.t, box.r, box.b};
    out.arrow = {arrow.l, arrow.t, arrow.r, arrow.b};
    // The text sits after the left margin and lead, centred in what is left when the box was made
    // wider than it, and halfway down the box counting 2 pixels more than its height.
    const int boxL = box.l - win.l, boxT = box.t - win.t, boxR = box.r - win.l;
    out.textX = kSideMargin + boxL + kTextLead;
    if (boxR - (out.textX + textW) > kSideMargin) {
        out.textX += (boxR - out.textX - kSideMargin - textW) / 2;
    }
    out.textY = (box.b - box.t - (textH + 2)) / 2 + 1 + boxT;
    return out;
}

tga::Pixels drawWorldTip(const tga::Pixels& sheet, const WorldTipLayout& layout) {
    tga::Pixels out;
    const int ww = layout.window[2] - layout.window[0], wh = layout.window[3] - layout.window[1];
    if (ww <= 0 || wh <= 0 || sheet.width < 5 || sheet.height < 5 || sheet.rgba.size() < std::size_t{sheet.width} * sheet.height * 4) {
        return out;
    }
    out.width = static_cast<std::uint32_t>(ww);
    out.height = static_cast<std::uint32_t>(wh);
    out.rgba.assign(static_cast<std::size_t>(ww) * static_cast<std::size_t>(wh) * 4, 0);
    const int cw = static_cast<int>(sheet.width) / 5, ch = static_cast<int>(sheet.height) / 5;
    const int l = layout.box[0] - layout.window[0], t = layout.box[1] - layout.window[1];
    const int r = layout.box[2] - layout.window[0], b = layout.box[3] - layout.window[1];
    // The box's rows between its top and bottom cells: from the sheet's second row as far as the
    // text's top margin reaches past the top cell, then its third row down to the bottom cell.
    auto row = [&](int sheetRow, int y0, int y1) {
        const int sy = sheetRow * ch;
        tile(sheet, {0, sy, cw, sy + ch}, out, {l, y0, l + cw, y1});
        tile(sheet, {3 * cw, sy, 4 * cw, sy + ch}, out, {r - cw, y0, r, y1});
        tile(sheet, {cw, sy, 2 * cw, sy + ch}, out, {l + cw, y0, r - cw, y1});
    };
    const int split = kEndMargin + 3;
    int upper = split - ch, lower = (b - t) - split - ch;
    if (lower < 0) {
        upper = (b - t) - 2 * ch;
        lower = 0;
    }
    if (upper > 0) {
        row(1, t + ch, t + ch + upper);
    }
    if (lower > 0) {
        row(2, b - ch - lower, b - ch);
    }
    // The corners, then the top and bottom edges between them. The bottom cells come after the top
    // ones, so in a box under 2 cells tall they cover the top cells' lower part.
    tile(sheet, {0, 0, cw, ch}, out, {l, t, l + cw, t + ch});
    tile(sheet, {3 * cw, 0, 4 * cw, ch}, out, {r - cw, t, r, t + ch});
    tile(sheet, {0, 3 * ch, cw, 4 * ch}, out, {l, b - ch, l + cw, b});
    tile(sheet, {3 * cw, 3 * ch, 4 * cw, 4 * ch}, out, {r - cw, b - ch, r, b});
    tile(sheet, {cw, 0, 2 * cw, ch}, out, {l + cw, t, r - cw, t + ch});
    tile(sheet, {cw, 3 * ch, 2 * cw, 4 * ch}, out, {l + cw, b - ch, r - cw, b});
    // The arrow, with the piece where it joins the box.
    const Rect cells[4] = {{2 * cw, 4 * ch, 4 * cw, 5 * ch}, {4 * cw, 2 * ch, 5 * cw, 4 * ch}, {0, 4 * ch, 2 * cw, 5 * ch}, {4 * cw, 0, 5 * cw, 2 * ch}};
    const Rect src = cells[std::clamp(layout.side, 0, 3)];
    const int ax = layout.arrow[0] - layout.window[0], ay = layout.arrow[1] - layout.window[1];
    tile(sheet, src, out, {ax, ay, ax + src.r - src.l, ay + src.b - src.t});
    return out;
}

} // namespace ts2::ui
