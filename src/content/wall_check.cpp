#include "terrain_edit.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::content {

namespace {

constexpr int kUnsupported = 0x48, kSideOffLot = 0x40, kNotDiagonal = 0x4C, kLocked = 0x45, kUneven = 0x49, kOnLotEdge = 0x45, kInPond = 0x47, kOnBeach = 0xA4, kTooSteep = 0x31,
              kBelowGround = 0x1C;
constexpr int kObjectInTheWay = 0x0C, kSimInTheWay = 0x4B, kSharesEdge = 0x41, kTouchesArch = 0x4D, kTouchesWall = 0x50, kTouchesOther = 0;
constexpr float kSteepest = 1.7320508f;  // tan 60 degrees
constexpr std::uint32_t kWall300 = 300, kWall301 = 301;

// The kinds a style may not share an edge with, and may not meet at an end, as bits by kind.
struct KindMasks {
    unsigned share = 0, touch = 0;
};
KindMasks masksOf(WallRules::Kind kind) {
    using Kind = WallRules::Kind;
    switch (kind) {
    case Kind::Wall:
    case Kind::HalfWall: return {0xF, 0x4};
    case Kind::Fence: return {0xB, 0};
    case Kind::FenceArch: return {0x5, 0x1};
    case Kind::Wall300: return {0x10, 0x4};
    case Kind::Wall301: return {0x20, 0x4};
    }
    return {};
}

TerrainCorner tileOf(const TileQuarter& q) {
    return {q.x, q.y, q.level};
}

// Whether the section runs along an edge of an object's tile that the object keeps walls off. Object
// data 0x0D bits 0x100, 0x200, 0x400 and 0x800 keep the four sides when set, and the two diagonals are
// kept unless bits 0x20 and 0x80 are set. The edges turn with the object by quarter turns.
bool ownsEdge(const TerrainObject& o, const WallSegment& s) {
    struct Edge {
        std::uint16_t bit;
        int from, to;  // tile corners by direction, odd from 1 to 7
        bool whenSet;
    };
    constexpr Edge kEdges[] = {{0x100, 3, 1, true}, {0x200, 1, 7, true}, {0x400, 7, 5, true}, {0x800, 5, 3, true},
                               {0x20, 7, 3, false}, {0x80, 1, 5, false}};
    const float tx = std::floor(o.x), ty = std::floor(o.y);
    const int turn = o.direction & 6;
    const auto corner = [&](int c) {
        const int i = (turn + c + 6) & 7;
        return std::pair{tx + (i == 5 || i == 7 ? 1.0f : 0.0f), ty + (i == 1 || i == 7 ? 1.0f : 0.0f)};
    };
    for (const auto& e : kEdges) {
        if (((o.wallFlags & e.bit) != 0) != e.whenSet) {
            continue;
        }
        const auto [ax, ay] = corner(e.from);
        const auto [bx, by] = corner(e.to);
        if ((ax == s.ax && ay == s.ay && bx == s.bx && by == s.by) || (ax == s.bx && ay == s.by && bx == s.ax && by == s.ay)) {
            return true;
        }
    }
    return false;
}

std::size_t edgesKept(const TerrainObject& o) {
    std::size_t n = 0;
    for (const unsigned bit : {0x100u, 0x200u, 0x400u, 0x800u}) {
        n += (o.wallFlags & bit) != 0 ? 1 : 0;
    }
    for (const unsigned bit : {0x20u, 0x80u}) {
        n += (o.wallFlags & bit) == 0 ? 1 : 0;
    }
    return n;
}

// The tiles a box covers once shrunk by a thousandth of a tile, and whether its low sides pass the
// middle of their tiles and its high sides stop short of it.
struct BoxTiles {
    std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool lowXPast = false, lowYPast = false, highXShort = false, highYShort = false;
};
BoxTiles tilesOf(float x0, float y0, float x1, float y1) {
    BoxTiles t;
    t.x0 = static_cast<std::int32_t>(std::floor(x0));
    t.y0 = static_cast<std::int32_t>(std::floor(y0));
    t.x1 = static_cast<std::int32_t>(std::floor(x1));
    t.y1 = static_cast<std::int32_t>(std::floor(y1));
    t.lowXPast = x0 - static_cast<float>(t.x0) > 0.5f;
    t.lowYPast = y0 - static_cast<float>(t.y0) > 0.5f;
    t.highXShort = x1 - static_cast<float>(t.x1) < 0.5f;
    t.highYShort = y1 - static_cast<float>(t.y1) < 0.5f;
    return t;
}

// The lines of tile (x, y) a box reaches, by bit: 1 the tile's low x side, 2 its low y side, 4 its
// rising diagonal, 8 its falling one. The box's own low sides don't count, and a corner of the box
// that doesn't reach the tile's middle misses the diagonal running past it.
unsigned linesReached(const BoxTiles& b, std::int32_t x, std::int32_t y) {
    unsigned m = x == b.x0 ? 0xE : 0xF;
    m ^= y == b.y0 ? 2 : 0;
    m ^= x == b.x0 && y == b.y0 && b.lowXPast && b.lowYPast ? 8 : 0;
    m ^= x == b.x1 && y == b.y1 && b.highXShort && b.highYShort ? 8 : 0;
    m ^= x == b.x0 && y == b.y1 && b.lowXPast && b.highYShort ? 4 : 0;
    m ^= x == b.x1 && y == b.y0 && b.highXShort && b.lowYPast ? 4 : 0;
    return m;
}

constexpr float kShrink = 0.001f;

// Whether a footprint rectangle reaches the diagonal (4 rising, 8 falling) of any tile it covers.
bool reachesDiagonal(const std::array<float, 4>& r, unsigned diagonal) {
    if (!(r[0] < r[2] && r[1] < r[3])) {
        return false;
    }
    const BoxTiles b = tilesOf(r[0] + kShrink, r[1] + kShrink, r[2] - kShrink, r[3] - kShrink);
    for (std::int32_t x = b.x0; x <= b.x1; ++x) {
        for (std::int32_t y = b.y0; y <= b.y1; ++y) {
            if (linesReached(b, x, y) & diagonal) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool TerrainEdit::boxCutByWall(const TerrainObject& o, std::int32_t level) const {
    bool any = false;
    std::array<float, 4> box{};
    for (const auto& r : o.footprint) {
        if (!(r[0] < r[2] && r[1] < r[3])) {
            continue;
        }
        box = any ? std::array<float, 4>{std::min(box[0], r[0]), std::min(box[1], r[1]), std::max(box[2], r[2]), std::max(box[3], r[3])} : r;
        any = true;
    }
    if (!any || level < bounds_.minLevel || level > bounds_.maxLevel) {
        return false;
    }
    // The shrunk box is kept to the lot's tiles.
    const auto inX = [&](float v) { return std::clamp(v, static_cast<float>(bounds_.minX), static_cast<float>(bounds_.maxX)); };
    const auto inY = [&](float v) { return std::clamp(v, static_cast<float>(bounds_.minY), static_cast<float>(bounds_.maxY)); };
    const BoxTiles b = tilesOf(inX(box[0] + kShrink), inY(box[1] + kShrink), inX(box[2] - kShrink), inY(box[3] - kShrink));
    for (std::int32_t x = b.x0; x <= b.x1; ++x) {
        for (std::int32_t y = b.y0; y <= b.y1; ++y) {
            const unsigned m = linesReached(b, x, y);
            const float fx = static_cast<float>(x), fy = static_cast<float>(y);
            const WallSegment lines[] = {{fx, fy, fx, fy + 1, level},
                                         {fx, fy, fx + 1, fy, level},
                                         {fx, fy, fx + 1, fy + 1, level},
                                         {fx + 1, fy, fx, fy + 1, level}};
            for (int i = 0; i < 4; ++i) {
                if ((m >> i & 1) && walls_.dividesOn(lines[i])) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool TerrainEdit::tileInside(const TerrainCorner& t) const {
    return t.x > bounds_.minX && t.x < bounds_.maxX && t.y > bounds_.minY && t.y < bounds_.maxY && t.level >= bounds_.minLevel &&
           t.level <= bounds_.maxLevel;
}

bool TerrainEdit::tileWet(const TerrainCorner& t) const {
    const auto w = lot_.flatGrids.find(kWaterHeightGridInstance);
    if (w == lot_.flatGrids.end() || t.level != 0) {
        return false;
    }
    for (const auto [dx, dy] : {std::pair{0, 0}, {1, 0}, {1, 1}, {0, 1}}) {
        const TerrainCorner c{t.x + dx, t.y + dy, t.level};
        if (!cornerInLot(c)) {
            continue;
        }
        const auto water = w->second.f32(c.x, c.y);
        if (water && height(c) < *water) {
            return true;
        }
    }
    return false;
}

WallRules::Kind TerrainEdit::kindOf(std::uint32_t style) const {
    using Kind = WallRules::Kind;
    if (const auto fence = walls_.fence(style)) {
        return fence->halfWall ? Kind::HalfWall : Kind::Fence;
    }
    return style == kWall300 ? Kind::Wall300 : style == kWall301 ? Kind::Wall301 : Kind::Wall;
}

// A diagonal may not cross a tile that has one already: a stage side's another stage side, any other
// style's any but walls 300 and 301.
bool TerrainEdit::crossesDiagonal(const TileQuarter& tile, bool stageSide) const {
    const float x = static_cast<float>(tile.x), y = static_cast<float>(tile.y);
    for (const WallSegment& d : {WallSegment{x, y, x + 1, y + 1, tile.level}, WallSegment{x + 1, y, x, y + 1, tile.level}}) {
        if (const auto style = walls_.styleOn(d)) {
            if (stageSide ? *style == kWall301 : *style != kWall300 && *style != kWall301) {
                return true;
            }
        }
    }
    return false;
}

// A side of the section holds it up when every quarter of it on the section's side is held up from
// right below, when a flat topped hip roof one storey down reaches at least three tiles past it on
// every side, when every such quarter has a floor and a surface, or when a wall of style 1, 3, 5 to
// 10, 15, 22 or 23 stands under the section. What styles 5 to 22 are is not known yet.
bool TerrainEdit::supported(const WallSegment& s) const {
    constexpr std::uint32_t kFlatTopHip = 6;
    constexpr std::int32_t kRoofMargin = 3;
    constexpr std::uint32_t kHoldingStyles[] = {1, 3, 5, 6, 7, 8, 9, 10, 15, 22, 23};
    const auto floors = lot_.grids.find(kFloorGridInstance);
    const auto surfaces = lot_.grids.find(kSurfaceGridInstance);
    for (const TileQuarter& side : {leftQuarter(s), leftQuarter(s.reversed())}) {
        if (!tileInLot(tileOf(side))) {
            continue;
        }
        bool heldUp = true, floored = floors != lot_.grids.end() && surfaces != lot_.grids.end() && floors->second.elementSize == 8 &&
                                      surfaces->second.elementSize == 4;
        const std::int32_t l = gridIndex(lot_, side.level), gx = side.x - bounds_.minX, gy = side.y - bounds_.minY;
        floored = floored && floors->second.contains(l, gx, gy) && surfaces->second.contains(l, gx, gy);
        for (int q = 0; q < 4; ++q) {
            if (!(side.mask >> q & 1)) {
                continue;
            }
            const TileQuarter one{side.x, side.y, side.level, static_cast<std::uint8_t>(1u << q)};
            heldUp = heldUp && (heldUp_ ? heldUp_(one) : side.level < 1);
            floored = floored && floors->second.i16(l, gx, gy, static_cast<std::size_t>(q)) != 0 &&
                      surfaces->second.at(l, gx, gy)[static_cast<std::size_t>(q)] != 0;
        }
        if (heldUp) {
            return true;
        }
        for (const auto& r : lot_.roofs) {
            const auto usable = [](float v) { return std::isfinite(v) && std::abs(v) < 1.0e6f; };
            if (r.type != kFlatTopHip || r.level0 != side.level - 1 || r.level0 < 0 || !usable(r.x0) || !usable(r.x1) || !usable(r.y0) ||
                !usable(r.y1)) {
                continue;  // a broken file's corners can't be made tile numbers
            }
            const auto minX = static_cast<std::int32_t>(std::min(r.x0, r.x1)), minY = static_cast<std::int32_t>(std::min(r.y0, r.y1));
            const auto maxX = static_cast<std::int32_t>(std::max(r.x0, r.x1)), maxY = static_cast<std::int32_t>(std::max(r.y0, r.y1));
            if (side.x >= minX + kRoofMargin && side.y >= minY + kRoofMargin && side.x < maxX - kRoofMargin && side.y < maxY - kRoofMargin) {
                return true;
            }
        }
        if (floored) {
            return true;
        }
        if (s.level > 0) {
            const auto below = walls_.styleOn({s.ax, s.ay, s.bx, s.by, s.level - 1});
            if (below && std::find(std::begin(kHoldingStyles), std::end(kHoldingStyles), *below) != std::end(kHoldingStyles)) {
                return true;
            }
        }
    }
    return false;
}

bool TerrainEdit::checkWall(const WallSegment& s, const WallRules& rules, std::vector<TerrainRefusal>& refusals) const {
    using Kind = WallRules::Kind;
    const std::size_t before = refusals.size();
    const TileQuarter a = leftQuarter(s), b = leftQuarter(s.reversed());
    const bool inA = tileInLot(tileOf(a)), inB = tileInLot(tileOf(b));
    const auto atTile = [&](const TileQuarter& q, int code) {
        refuse(&refusals, code, static_cast<float>(q.x) + 0.5f, static_cast<float>(q.y) + 0.5f, q.level);
    };
    const float midX = (s.ax + s.bx) / 2, midY = (s.ay + s.by) / 2;
    const auto atMiddle = [&](int code) { refuse(&refusals, code, midX, midY, s.level); };

    // A wall needs both its sides on the lot, a fence or half wall one of them.
    const bool fence = rules.kind == Kind::Fence || rules.kind == Kind::HalfWall;
    if (!inA && !fence) {
        atTile(a, kSideOffLot);
        return false;
    }
    if (!inB && (!fence || !inA)) {
        atTile(b, kSideOffLot);
        return false;
    }
    const bool diagonal = s.ax != s.bx && s.ay != s.by;
    if (rules.mayPlaceDiagonally && diagonal && crossesDiagonal(a, rules.kind == Kind::Wall301)) {
        atMiddle(kNotDiagonal);
    }
    // What already stands on the edge, by kind: arches in their layer, stage sides, and the rest by
    // their style. Wall 300 is never found.
    const KindMasks masks = masksOf(rules.kind);
    const auto standing = walls_.styleOn(s);
    for (int i = 0; i < 6; ++i) {
        const auto k = static_cast<Kind>(i);
        const bool there = k == Kind::FenceArch ? walls_.archOn(s)
                           : k == Kind::Wall301 ? standing == kWall301
                           : k == Kind::Wall300 ? false
                                                : standing && *standing != kWall301 && kindOf(*standing) == k;
        if ((masks.share >> i & 1) && there) {
            atMiddle(kSharesEdge);
        }
        if (!(masks.touch >> i & 1)) {
            continue;
        }
        // The first piece of that kind at the section's start, else at its end.
        std::optional<WallSegment> met;
        for (const auto& [x, y] : {std::pair{s.ax, s.ay}, std::pair{s.bx, s.by}}) {
            if (k == Kind::FenceArch) {
                const auto arches = walls_.archesAt(x, y, s.level);
                if (!arches.empty()) {
                    met = arches.front();
                }
            } else {
                for (const auto& w : walls_.wallsAt(x, y, s.level)) {
                    if (w.style != kWall300 && w.style != kWall301 && kindOf(w.style) == k) {
                        met = w.segment;
                        break;
                    }
                }
            }
            if (met) {
                break;
            }
        }
        if (met) {
            const bool wallKind = rules.kind == Kind::Wall || rules.kind == Kind::HalfWall;
            const int code = wallKind && k == Kind::FenceArch ? kTouchesArch : rules.kind == Kind::FenceArch && k == Kind::Wall ? kTouchesWall : kTouchesOther;
            refuse(&refusals, code, (met->ax + met->bx) / 2, (met->ay + met->by) / 2, met->level);
        }
    }
    if (!rules.mayPlaceDiagonally && diagonal) {
        atMiddle(kNotDiagonal);
    }
    // A side off the lot counts as locked. A wall needs neither side locked, a fence one side free.
    const bool lockedA = !inA || tileLocked(tileOf(a)), lockedB = !inB || tileLocked(tileOf(b));
    if (fence ? lockedA && lockedB : lockedA || lockedB) {
        atTile(a, kLocked);
    }
    const auto end = [&](float x, float y) { return TerrainCorner{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), s.level}; };
    if (rules.flatBottom && height(end(s.ax, s.ay)) != height(end(s.bx, s.by))) {
        atMiddle(kUneven);
    }
    if (rules.supportUnderneath && !supported(s)) {
        atMiddle(kUnsupported);
    }
    (void)overPool(s, refusals);
    if (!rules.submersible && (!inA || tileWet(tileOf(a))) && (!inB || tileWet(tileOf(b)))) {
        atTile(a, kInPond);
    }
    if (beachBad(end(s.ax, s.ay)) && beachBad(end(s.bx, s.by))) {
        atTile(a, kOnBeach);
    }
    if (refusals.size() != before) {
        return false;
    }

    // What stands on each side's tile, but not for wall 300 or a fence arch. Hidden objects are passed
    // over, a Sim ends that side's look, and a half wall passes objects with data 0x2A bit 0x1000. An
    // object is in the way when it keeps the section's edge, when the section is a one tile diagonal
    // its footprint reaches, or, for a style that blocks walking, when it keeps an edge, is tested by
    // its box and a wall dividing rooms already runs inside that box. Objects the game keeps at the
    // section's ends are not checked yet.
    const bool unitDiagonal = std::fabs(s.bx - s.ax) == 1 && std::fabs(s.by - s.ay) == 1;
    const unsigned diagonalBit = (s.bx - s.ax) * (s.by - s.ay) > 0 ? 4 : 8;
    constexpr std::uint16_t kUnderHalfWalls = 0x1000;
    const std::size_t beforeObjects = refusals.size();
    for (const TileQuarter& q : {a, b}) {
        if (!objects_ || rules.kind == Kind::Wall300 || rules.kind == Kind::FenceArch || !tileInLot(tileOf(q))) {
            continue;
        }
        for (const auto& o : objects_(q.x, q.y, q.level)) {
            if (o.hidden != 0) {
                continue;
            }
            if (o.sim) {
                refuse(&refusals, kSimInTheWay, o.x, o.y, q.level);
                break;
            }
            if (rules.kind == Kind::HalfWall && (o.placement & kUnderHalfWalls) != 0) {
                continue;
            }
            const bool crossed = unitDiagonal && std::any_of(o.footprint.begin(), o.footprint.end(),
                                                             [&](const auto& r) { return reachesDiagonal(r, diagonalBit); });
            if (ownsEdge(o, s) || crossed || (rules.blocksLocomotion && edgesKept(o) > 0 && o.byBox && boxCutByWall(o, q.level))) {
                refuse(&refusals, kObjectInTheWay, o.x, o.y, q.level);
            }
        }
    }
    if (refusals.size() != beforeObjects) {
        return false;
    }
    // A one tile diagonal over a deck or foundation piece is left out without a reason.
    if (rules.kind != Kind::Wall300 && rules.kind != Kind::FenceArch && unitDiagonal && pieceOn(tileOf(a))) {
        return false;
    }

    // Walls stay off the lot's outer ring of tiles. Fences and half walls may not be steeper than 60
    // degrees or below the ground storey.
    const auto inside = [&] {
        if (tileInside(tileOf(a)) && tileInside(tileOf(b))) {
            return true;
        }
        atMiddle(kOnLotEdge);
        return false;
    };
    const auto ground = [&] {
        const float length = std::hypot(s.bx - s.ax, s.by - s.ay);
        if (length > 0 && std::fabs(height(end(s.bx, s.by)) - height(end(s.ax, s.ay))) / length > kSteepest) {
            atMiddle(kTooSteep);
        }
        if (s.level < 0) {
            atMiddle(kBelowGround);
        }
        return refusals.size() == before;
    };
    switch (rules.kind) {
    case Kind::Wall:
    case Kind::Wall301:
        return inside();
    case Kind::HalfWall:
        return inside() && ground();
    case Kind::Fence:
        return ground();
    case Kind::Wall300:
        return true;
    case Kind::FenceArch:
        // A fence arch's own checks are not done yet.
        return true;
    }
    return true;
}

} // namespace ts2::content
