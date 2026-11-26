#include "content/floor_edit.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <set>
#include <span>
#include <tuple>

namespace ts2::content {

namespace {

constexpr std::size_t kQuarters = 4;

bool sameName(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char p, unsigned char q) {
               return std::tolower(p) == std::tolower(q);
           });
}

// The pattern map's id for `name`, which it gains when it has none. 0 for no floor.
std::uint16_t patternId(Lot& lot, const std::string& name) {
    if (name.empty()) {
        return 0;
    }
    for (const auto& [id, entry] : lot.floorPatterns) {
        if (sameName(entry.name, name)) {
            return id;
        }
    }
    // A new name takes the smallest id not in use, from 1. A full map, which no real lot has, gives no floor.
    std::uint16_t id = 1;
    while (lot.floorPatterns.contains(id)) {
        if (id == UINT16_MAX) {
            return 0;
        }
        ++id;
    }
    lot.floorPatterns.emplace(id, stringmap::Entry{name, 0});
    return id;
}

} // namespace

std::string floorPatternAt(const Lot& lot, std::int32_t level, std::int32_t x, std::int32_t y, int quarter) {
    const auto floors = lot.grids.find(kFloorGridInstance);
    if (floors == lot.grids.end() || floors->second.elementSize != 2 * kQuarters || quarter < 0 || quarter >= static_cast<int>(kQuarters) ||
        !floors->second.contains(level, x, y)) {
        return {};
    }
    const auto id = static_cast<std::uint16_t>(floors->second.i16(level, x, y, static_cast<std::size_t>(quarter)));
    const auto entry = lot.floorPatterns.find(id);
    return id == 0 || entry == lot.floorPatterns.end() ? std::string() : entry->second.name;
}

std::vector<FloorQuarter> setFloors(Lot& lot, const std::vector<FloorQuarter>& quarters) {
    std::vector<FloorQuarter> was;
    const auto floors = lot.grids.find(kFloorGridInstance);
    if (floors == lot.grids.end() || floors->second.elementSize != 2 * kQuarters) {
        return was;
    }
    auto& grid = floors->second;
    const auto turns = lot.grids.find(kFloorTurnGridInstance);
    for (const auto& q : quarters) {
        if (q.quarter < 0 || q.quarter >= static_cast<int>(kQuarters) || !grid.contains(q.level, q.x, q.y)) {
            continue;
        }
        FloorQuarter before = q;
        before.pattern = floorPatternAt(lot, q.level, q.x, q.y, q.quarter);
        const bool turned = turns != lot.grids.end() && turns->second.elementSize == 2 && turns->second.contains(q.level, q.x, q.y);
        if (turned) {
            const auto t = turns->second.at(q.level, q.x, q.y);
            before.turn = (t[static_cast<std::size_t>(q.quarter) / 2] >> (4 * (q.quarter % 2))) & 0xF;
        }
        // Only a different pattern changes the quarter. A name no quarter uses any more leaves the map.
        const bool changed = !sameName(before.pattern, q.pattern);
        was.push_back(std::move(before));
        auto cell = grid.at(q.level, q.x, q.y);
        const auto at = static_cast<std::size_t>(q.quarter) * 2;
        const auto old = static_cast<std::uint16_t>(cell[at] | cell[at + 1] << 8);
        if (changed) {
            const auto id = patternId(lot, q.pattern);
            if (const auto e = lot.floorPatterns.find(id); id != 0 && e != lot.floorPatterns.end()) {
                ++e->second.uses;
            }
            // A broken file's grid may hold an id the map lacks, which the new name may just have taken.
            if (const auto e = lot.floorPatterns.find(old);
                old != 0 && old != id && e != lot.floorPatterns.end() && e->second.uses > 0 && --e->second.uses == 0) {
                lot.floorPatterns.erase(e);
            }
            cell[at] = static_cast<std::uint8_t>(id & 0xFF);
            cell[at + 1] = static_cast<std::uint8_t>(id >> 8);
        }
        // A nibble per quarter, in the floor grid's order.
        if (turned) {
            auto t = turns->second.at(q.level, q.x, q.y);
            auto& byte = t[static_cast<std::size_t>(q.quarter) / 2];
            const int shift = 4 * (q.quarter % 2);
            byte = static_cast<std::uint8_t>((byte & ~(0xF << shift)) | (q.turn & 0xF) << shift);
        }
    }
    return was;
}

namespace {

std::span<std::uint8_t> cornerAt(Lot& lot, std::int32_t level, std::int32_t x, std::int32_t y) {
    const auto heights = lot.grids.find(kFloorHeightGridInstance);
    if (heights == lot.grids.end() || heights->second.elementSize != sizeof(float) || !heights->second.contains(level, x, y)) {
        return {};
    }
    return heights->second.at(level, x, y);
}

std::optional<float> heightAt(Lot& lot, std::int32_t level, std::int32_t x, std::int32_t y) {
    const auto cell = cornerAt(lot, level, x, y);
    if (cell.size() != sizeof(float)) {
        return std::nullopt;
    }
    float z = 0;
    std::memcpy(&z, cell.data(), sizeof z);
    return z;
}

void setHeight(Lot& lot, std::int32_t level, std::int32_t x, std::int32_t y, float z) {
    const auto cell = cornerAt(lot, level, x, y);
    if (cell.size() == sizeof(float)) {
        std::memcpy(cell.data(), &z, sizeof z);
    }
}

} // namespace

std::vector<CornerHeight> levelAroundFloors(Lot& lot, const std::vector<FloorQuarter>& laid, std::int32_t ground) {
    std::vector<CornerHeight> was;
    const auto heights = lot.grids.find(kFloorHeightGridInstance);
    if (heights == lot.grids.end()) {
        return was;
    }
    const auto levels = static_cast<std::int32_t>(heights->second.levels);
    // Each corner is kept once, as it first was, for undo.
    std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t>> kept;
    for (const auto& q : laid) {
        if (q.pattern.empty() || q.level <= ground) {
            continue;
        }
        const auto z = heightAt(lot, q.level, q.x, q.y);
        if (!z) {
            continue;
        }
        for (std::int32_t cx = q.x - 1; cx <= q.x + 2; ++cx) {
            for (std::int32_t cy = q.y - 1; cy <= q.y + 2; ++cy) {
                const auto corner = heightAt(lot, q.level, cx, cy);
                if (!corner || *corner == *z) {
                    continue;
                }
                const float by = *z - *corner;
                for (std::int32_t l = q.level; l < levels; ++l) {
                    const auto above = heightAt(lot, l, cx, cy);
                    if (!above) {
                        continue;
                    }
                    if (kept.insert({l, cx, cy}).second) {
                        was.push_back({l, cx, cy, *above});
                    }
                    setHeight(lot, l, cx, cy, *above + by);
                }
            }
        }
    }
    return was;
}

void setCornerHeights(Lot& lot, const std::vector<CornerHeight>& corners) {
    for (const auto& c : corners) {
        setHeight(lot, c.level, c.x, c.y, c.z);
    }
}

} // namespace ts2::content
