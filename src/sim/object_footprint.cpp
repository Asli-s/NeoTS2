#include "sim/object_footprint.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace ts2::sim {

namespace {

constexpr float kStep = 0.125f;   // a mask cell, after halving the 16x16 mask
constexpr float kInset = 0.01f;   // rectangles stay this far inside their cells
constexpr std::size_t kMaskBytes = 32;  // 16 rows of 16 bits
// Multi-tile objects are a few tiles across; anything further from its lead is bad data.
constexpr int kMaxTileOffset = 64;

// A tile's 16x16 mask halved to 8x8: each cell is set when any of its four bits is. Bit c of row r
// is column c.
std::array<std::uint8_t, 8> halveMask(const std::vector<std::uint8_t>& bytes) {
    std::array<std::uint8_t, 8> rows{};
    if (bytes.size() != kMaskBytes) {
        return rows;
    }
    for (int r = 0; r < 16; ++r) {
        const unsigned bits = bytes[2 * r] | (static_cast<unsigned>(bytes[2 * r + 1]) << 8);
        for (int c = 0; c < 16; ++c) {
            if (bits & (1u << c)) {
                rows[r / 2] = static_cast<std::uint8_t>(rows[r / 2] | (1u << (c / 2)));
            }
        }
    }
    return rows;
}

// The masks for `graphic`: the list named after it, graphic 0 the first list by name.
const scenegraph::DataValue* graphicMasks(const scenegraph::DataValue& footprint, int graphic) {
    if (graphic != 0) {
        return footprint.find(std::to_string(graphic));
    }
    const scenegraph::DataValue* first = nullptr;
    for (const auto& item : footprint.items) {
        if (item.type == scenegraph::DataValue::Type::List && (first == nullptr || item.name < first->name)) {
            first = &item;
        }
    }
    return first;
}

} // namespace

std::vector<RouteRect> footprintTile(const scenegraph::DataValue& footprint, int graphic, int tileX, int tileY) {
    std::vector<RouteRect> rects;
    if (std::abs(tileX) > kMaxTileOffset || std::abs(tileY) > kMaxTileOffset) {
        return rects;
    }
    const auto* masks = graphicMasks(footprint, graphic);
    // Tiles are named in model coordinates.
    const auto* mask = masks ? masks->find("(" + std::to_string(tileX) + "," + std::to_string(-tileY) + ")") : nullptr;
    if (mask == nullptr || mask->bytes.size() != kMaskBytes) {
        return rects;
    }
    const auto rows = halveMask(mask->bytes);
    // Rows that are alike merge into one band; each run of set cells in a band is a rectangle.
    int bandStart = 0;
    for (int r = 1; r <= 8; ++r) {
        if (r < 8 && rows[r] == rows[bandStart]) {
            continue;
        }
        const unsigned bits = rows[bandStart];
        for (int c = 0; c < 8;) {
            if (!(bits & (1u << c))) {
                ++c;
                continue;
            }
            const int runStart = c;
            while (c < 8 && (bits & (1u << c))) {
                ++c;
            }
            // Rows run from the tile's high-y edge down.
            rects.push_back({runStart * kStep - 0.5f + kInset, 0.5f - r * kStep + kInset, c * kStep - 0.5f - kInset,
                             0.5f - bandStart * kStep - kInset});
        }
        bandStart = r;
    }
    return rects;
}

RouteRect doorwayRect() {
    constexpr float kHalf = 0.2f;  // a 0.4 square on the wall, half a tile from the door's centre
    return {-kHalf, 0.5f - kHalf, kHalf, 0.5f + kHalf};
}

std::vector<RouteRect> footprintRects(const FootprintShape& shape) {
    if (shape.square) {
        const float half = shape.squareSize / 2;
        auto inset = [&](int side) { return static_cast<float>((shape.insets >> (4 * side)) & 0xF) / 16; };
        return {{-half + inset(1), -half + inset(0), half - inset(3), half - inset(2)}};
    }
    if (shape.masks != nullptr) {
        return footprintTile(*shape.masks, shape.graphic, shape.tileX, shape.tileY);
    }
    if (shape.modelBox) {
        // The model's extent, mirrored into route coordinates, around this tile and cut to it.
        const RouteRect& b = *shape.modelBox;
        const float tx = static_cast<float>(shape.tileX), ty = static_cast<float>(shape.tileY);
        return {{std::max(b.x0 - tx, -0.5f), std::max(-b.y1 - ty, -0.5f), std::min(b.x1 - tx, 0.5f), std::min(-b.y0 - ty, 0.5f)}};
    }
    return {};
}

FootprintRouting footprintRouting(std::uint16_t flags, std::int16_t cost, std::uint16_t flags2, int objectType, bool allowsIntersection) {
    constexpr int kWindowType = 9;
    FootprintRouting r;
    r.dynamic = allowsIntersection;
    if ((flags & 0x14) != 0 && objectType != kWindowType) {
        // Walk-through objects (flower beds, mail on a table): data 0x0A is the extra cost in hundredths.
        if (cost < 1 && !allowsIntersection) {
            r.added = false;
            return r;
        }
        r.passable = true;
        r.cost = static_cast<float>(cost) * 0.01f + 1;
        r.type = 0;
        return r;
    }
    // Blocking. Seats, tables and counters (flags2 bit 0x1000) get their own types.
    const bool seatOrSurface = (flags & 0x1) != 0 || (flags2 & 0x1000) != 0;
    r.type = allowsIntersection ? (seatOrSurface ? 3 : 5) : (seatOrSurface ? 9 : 11);
    return r;
}

bool footprintTileOf(float leadX, float leadY, int leadDirection, float x, float y, int& tileX, int& tileY) {
    tileX = tileY = 0;
    const float fx = x - leadX, fy = y - leadY;
    if (!std::isfinite(fx) || !std::isfinite(fy) || std::abs(fx) > kMaxTileOffset || std::abs(fy) > kMaxTileOffset) {
        return false;
    }
    // The part's offset from the lead, turned back (the inverse of the turns in placeFootprint).
    const int dx = static_cast<int>(std::lround(fx)), dy = static_cast<int>(std::lround(fy));
    switch ((leadDirection & 7) / 2) {
    case 0: tileX = dx; tileY = dy; break;
    case 1: tileX = dy; tileY = -dx; break;
    case 2: tileX = -dx; tileY = -dy; break;
    default: tileX = -dy; tileY = dx; break;
    }
    return true;
}

std::vector<RouteRect> placeFootprint(const std::vector<RouteRect>& local, float x, float y, int direction) {
    // A rectangle edge snapped to the nearest eighth, kept inside the original edge and kInset off
    // the grid line (turning can leave edges a little off).
    auto snapLow = [](float v) { return std::max(v, std::floor(v / kStep + 0.5f) * kStep + kInset); };
    auto snapHigh = [](float v) { return std::min(v, std::ceil(v / kStep - 0.5f) * kStep - kInset); };
    std::vector<RouteRect> placed;
    for (const auto& r : local) {
        RouteRect w;
        switch ((direction & 7) / 2) {
        case 0: w = {x + r.x0, y + r.y0, x + r.x1, y + r.y1}; break;
        case 1: w = {x - r.y1, y + r.x0, x - r.y0, y + r.x1}; break;
        case 2: w = {x - r.x1, y - r.y1, x - r.x0, y - r.y0}; break;
        default: w = {x + r.y0, y - r.x1, x + r.y1, y - r.x0}; break;
        }
        RouteRect s{snapLow(w.x0), snapLow(w.y0), snapHigh(w.x1), snapHigh(w.y1)};
        // A sliver keeps one cell.
        if (w.x0 < w.x1 && s.x1 - s.x0 < kStep / 2) {
            s.x1 = s.x0 + kStep - 2 * kInset;
        }
        if (w.y0 < w.y1 && s.y1 - s.y0 < kStep / 2) {
            s.y1 = s.y0 + kStep - 2 * kInset;
        }
        // Bad positions or model sizes (not finite) give nothing rather than a rectangle routing can't use.
        if (std::isfinite(s.x0) && std::isfinite(s.y0) && std::isfinite(s.x1) && std::isfinite(s.y1)) {
            placed.push_back(s);
        }
    }
    return placed;
}

} // namespace ts2::sim
