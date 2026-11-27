#include "floor_tool.hpp"

#include "scene/lot_parts.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace app {

namespace {

constexpr int kQuarters = 4;

bool sameName(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char p, unsigned char q) {
               return std::tolower(p) == std::tolower(q);
           });
}
constexpr int kLowX = 0, kHighY = 1, kHighX = 2, kLowY = 3;

std::optional<std::int16_t> roomOf(const ts2::content::Lot& lot, const FloorSpot& s) {
    const auto rooms = lot.grids.find(ts2::content::kRoomGridInstance);
    if (rooms == lot.grids.end() || rooms->second.elementSize != 2 * kQuarters || !rooms->second.contains(s.level, s.x, s.y)) {
        return std::nullopt;
    }
    return rooms->second.i16(s.level, s.x, s.y, static_cast<std::size_t>(s.quarter));
}

} // namespace

bool floorFillable(const ts2::content::Lot& lot, const FloorSpot& from) {
    const auto room = roomOf(lot, from);
    return room && (*room > 0 || !ts2::content::floorPatternAt(lot, from.level, from.x, from.y, from.quarter).empty());
}

FloorSpot floorSpotAt(float x, float y, std::int32_t level) {
    FloorSpot s{level, static_cast<std::int32_t>(std::floor(x)), static_cast<std::int32_t>(std::floor(y)), kLowX};
    const float u = x - static_cast<float>(s.x), v = y - static_cast<float>(s.y);
    float nearest = u;
    for (const auto [d, q] : {std::pair{1 - v, kHighY}, std::pair{1 - u, kHighX}, std::pair{v, kLowY}}) {
        if (d < nearest) {
            nearest = d;
            s.quarter = q;
        }
    }
    return s;
}

ts2::sim::FloorSupport floorSupport(const ts2::content::Lot& lot, ts2::scene::Assets& assets) {
    constexpr std::size_t kBuildSubsortField = 0x4A;
    constexpr std::uint16_t kColumns = 8;
    const auto& scripts = assets.buildScripts();
    return ts2::sim::FloorSupport(
        lot,
        [&](std::uint32_t type) -> std::optional<ts2::sim::PartitionSupport> {
            if (const auto style = scripts.wallStyles.find(type); style != scripts.wallStyles.end()) {
                return ts2::sim::PartitionSupport{style->second.bearsLoad, false};
            }
            if (const auto* fence = assets.fence(type)) {
                return ts2::sim::PartitionSupport{fence->bearsLoad, true};
            }
            return std::nullopt;
        },
        [&](std::uint32_t guid) {
            const auto* definition = assets.objectDefinition(guid);
            return definition != nullptr && definition->field(kBuildSubsortField) == kColumns;
        });
}

bool floorSurfaceAt(const ts2::content::Lot& lot, const FloorSpot& s) {
    constexpr std::size_t kByteSize = 1;
    const auto surface = lot.grids.find(ts2::content::kSurfaceGridInstance);
    if (surface == lot.grids.end() || surface->second.elementSize != kByteSize * kQuarters || !surface->second.contains(s.level, s.x, s.y)) {
        return true;
    }
    return !ts2::scene::surfaceQuarterOpen(surface->second, s.level, s.x, s.y, static_cast<std::size_t>(s.quarter));
}

std::vector<ts2::content::FloorQuarter> floorStroke(const ts2::content::Lot& lot, const FloorSpot& from, const FloorSpot& to,
                                                     const std::string& pattern, FloorStrokeShape shape,
                                                     const std::function<int(std::int32_t, std::int32_t, std::int32_t)>& diagonal,
                                                     const std::function<bool(std::int32_t, std::int32_t, std::int32_t)>& allowed) {
    std::vector<FloorSpot> spots;
    if (shape.quarters) {
        spots.push_back(to);
    } else if (shape.room) {
        const auto room = roomOf(lot, from);
        const auto floors = lot.grids.find(ts2::content::kFloorGridInstance);
        // Shift fills only the quarters with the clicked quarter's floor.
        const std::string clicked = ts2::content::floorPatternAt(lot, from.level, from.x, from.y, from.quarter);
        const auto sameFloor = [&](const FloorSpot& s) { return sameName(ts2::content::floorPatternAt(lot, s.level, s.x, s.y, s.quarter), clicked); };
        if (room && *room > 0 && floors != lot.grids.end()) {
            for (std::int32_t x = 0; x < static_cast<std::int32_t>(floors->second.width); ++x) {
                for (std::int32_t y = 0; y < static_cast<std::int32_t>(floors->second.height); ++y) {
                    for (int q = 0; q < kQuarters; ++q) {
                        const FloorSpot s{from.level, x, y, q};
                        if (roomOf(lot, s) == room && sameFloor(s)) {
                            spots.push_back(s);
                        }
                    }
                }
            }
        } else if (room && floorFillable(lot, from)) {
            // Outside a room, the joined quarters with that floor, as on a deck.
            const auto floored = [&](const FloorSpot& s) { return roomOf(lot, s) == room && sameFloor(s); };
            std::set<std::tuple<std::int32_t, std::int32_t, int>> seen{{from.x, from.y, from.quarter}};
            std::vector<FloorSpot> open{from};
            while (!open.empty()) {
                const FloorSpot s = open.back();
                open.pop_back();
                spots.push_back(s);
                // A quarter meets the two others of its tile beside it and the quarter across its edge.
                const int dx = s.quarter == kLowX ? -1 : s.quarter == kHighX ? 1 : 0;
                const int dy = s.quarter == kLowY ? -1 : s.quarter == kHighY ? 1 : 0;
                for (const FloorSpot n : {FloorSpot{s.level, s.x, s.y, (s.quarter + 1) % kQuarters}, FloorSpot{s.level, s.x, s.y, (s.quarter + 3) % kQuarters},
                                          FloorSpot{s.level, s.x + dx, s.y + dy, (s.quarter + 2) % kQuarters}}) {
                    if (seen.insert({n.x, n.y, n.quarter}).second && floored(n)) {
                        open.push_back(n);
                    }
                }
            }
        }
    } else if (from.x == to.x && from.y == to.y && diagonal && diagonal(to.level, to.x, to.y) != 0) {
        // A wall from the low corner parts the low-y and high-x quarters from the others, the other wall
        // the low edges' quarters from the high.
        const bool rising = diagonal(to.level, to.x, to.y) == 1;
        const auto half = [&](int q) { return rising ? q == kLowY || q == kHighX : q == kLowX || q == kLowY; };
        for (int q = 0; q < kQuarters; ++q) {
            if (half(q) == half(to.quarter)) {
                spots.push_back({to.level, to.x, to.y, q});
            }
        }
    } else {
        for (std::int32_t x = std::min(from.x, to.x); x <= std::max(from.x, to.x); ++x) {
            for (std::int32_t y = std::min(from.y, to.y); y <= std::max(from.y, to.y); ++y) {
                for (int q = 0; q < kQuarters; ++q) {
                    spots.push_back({from.level, x, y, q});
                }
            }
        }
    }
    std::vector<ts2::content::FloorQuarter> stroke;
    for (const auto& s : spots) {
        const auto was = ts2::content::floorPatternAt(lot, s.level, s.x, s.y, s.quarter);
        // Only a quarter getting a floor where it had none is checked, as the game lays a deck's floor
        // again over the objects on it.
        if (!floorSurfaceAt(lot, s) || sameName(was, pattern) || (was.empty() && allowed && !allowed(s.level, s.x, s.y))) {
            continue;
        }
        stroke.push_back({s.level, s.x, s.y, s.quarter, pattern, 1});
    }
    return stroke;
}

namespace {

// The platform a floor quarter tops: its kind in the platform region grid, none for no platform.
std::optional<std::int32_t> platformUnder(const ts2::content::Lot& lot, const ts2::content::FloorQuarter& q) {
    constexpr std::size_t kCellSize = 4 * kQuarters;
    // Pools (3, 5) and stages (4) have their region on their floor's storey, the foundation (0) and
    // decks (1, 2) one storey down.
    constexpr std::array<std::int32_t, 3> kOnItsStorey{3, 4, 5};
    constexpr std::array<std::int32_t, 3> kUnderIt{0, 1, 2};
    const auto among = [](const auto& kinds, std::optional<std::int32_t> r) { return r && std::find(kinds.begin(), kinds.end(), *r) != kinds.end(); };
    const auto regions = lot.grids.find(ts2::content::kPlatformRegionGridInstance);
    if (regions == lot.grids.end() || regions->second.elementSize != kCellSize) {
        return std::nullopt;
    }
    const auto& grid = regions->second;
    const auto regionAt = [&](std::int32_t level) -> std::optional<std::int32_t> {
        if (level < 0 || !grid.contains(level, q.x, q.y)) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(grid.u32(level, q.x, q.y, static_cast<std::size_t>(q.quarter)));
    };
    if (const auto region = regionAt(q.level); among(kOnItsStorey, region)) {
        return region;
    }
    if (const auto region = regionAt(q.level - 1); among(kUnderIt, region)) {
        return region;
    }
    return std::nullopt;
}

} // namespace

bool platformTile(const ts2::content::Lot& lot, std::int32_t level, std::int32_t x, std::int32_t y) {
    for (int quarter = 0; quarter < kQuarters; ++quarter) {
        if (platformUnder(lot, {level, x, y, quarter})) {
            return true;
        }
    }
    return false;
}

std::vector<ts2::content::FloorQuarter> keepPlatformFloors(const ts2::content::Lot& lot, std::vector<ts2::content::FloorQuarter> stroke,
                                                            const std::map<std::int32_t, std::string>& platformFloors) {
    std::vector<ts2::content::FloorQuarter> kept;
    kept.reserve(stroke.size());
    for (auto q : stroke) {
        const auto region = platformUnder(lot, q);
        const auto top = region ? platformFloors.find(*region) : platformFloors.end();
        if (q.pattern.empty() && top != platformFloors.end()) {
            q.pattern = top->second;
            if (sameName(ts2::content::floorPatternAt(lot, q.level, q.x, q.y, q.quarter), q.pattern)) {
                continue;
            }
        }
        kept.push_back(std::move(q));
    }
    return kept;
}

float floorStrokeCost(const ts2::content::Lot& lot, const std::vector<ts2::content::FloorQuarter>& stroke, FloorStroke kind,
                      const std::function<std::optional<int>(const std::string&)>& price) {
    constexpr float kQuarter = 0.25f, kSellBack = 0.8f;
    // A room can hold thousands of quarters, so each price is looked up once.
    std::map<std::string, std::optional<int>> prices;
    const auto priceOf = [&](const std::string& name) {
        auto it = prices.find(name);
        if (it == prices.end()) {
            it = prices.emplace(name, price(name)).first;
        }
        return it->second;
    };
    float cost = 0;
    for (const auto& q : stroke) {
        if (kind == FloorStroke::Lay) {
            cost += static_cast<float>(priceOf(q.pattern).value_or(0)) * kQuarter;
            continue;
        }
        const auto was = ts2::content::floorPatternAt(lot, q.level, q.x, q.y, q.quarter);
        if (!was.empty()) {
            cost -= std::floor(static_cast<float>(priceOf(was).value_or(0)) * kSellBack) * kQuarter;
        }
    }
    return cost;
}

} // namespace app
