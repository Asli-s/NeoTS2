#include "content/floor_edit.hpp"

#include <algorithm>
#include <format>
#include <limits>

namespace ts2::content {

namespace {

constexpr std::size_t kQuarters = 4;

std::uint16_t quarterAt(const array3d::Array3D& floors, std::int32_t level, std::int32_t x, std::int32_t y, std::size_t q) {
    return static_cast<std::uint16_t>(floors.i16(level, x, y, q));
}

void setQuarter(array3d::Array3D& floors, std::int32_t level, std::int32_t x, std::int32_t y, std::size_t q, std::uint16_t id) {
    const std::size_t index = (static_cast<std::size_t>(level) * floors.width + static_cast<std::size_t>(x)) * floors.height +
                              static_cast<std::size_t>(y);
    const std::size_t at = index * floors.elementSize + q * 2;
    if (at + 1 < floors.elements.size()) {
        floors.elements[at] = static_cast<std::uint8_t>(id & 0xFF);
        floors.elements[at + 1] = static_cast<std::uint8_t>(id >> 8);
    }
}

bool hasSurface(const array3d::Array3D* surface, std::int32_t level, std::int32_t x, std::int32_t y, std::size_t q) {
    if (surface == nullptr || surface->elementSize != kQuarters) {
        return true;
    }
    const auto quarters = surface->at(level, x, y);
    return quarters.size() != kQuarters || quarters[q] != 0;
}

// Calls `f(x, y)` for each tile of the rectangle on the grid.
template <typename F>
void eachTile(const array3d::Array3D& floors, std::int32_t level, TileRect rect, F&& f) {
    if (floors.elementSize != kQuarters * 2) {
        return;
    }
    const std::int32_t x0 = std::max(std::min(rect.x0, rect.x1), 0), x1 = std::max(rect.x0, rect.x1);
    const std::int32_t y0 = std::max(std::min(rect.y0, rect.y1), 0), y1 = std::max(rect.y0, rect.y1);
    for (std::int32_t x = x0; x <= x1; ++x) {
        for (std::int32_t y = y0; y <= y1; ++y) {
            if (floors.contains(level, x, y)) {
                f(x, y);
            }
        }
    }
}

// The pattern list keeps how many quarters use each pattern.
void countUse(std::map<std::uint16_t, stringmap::Entry>& patterns, std::uint16_t id, int delta) {
    const auto it = patterns.find(id);
    if (id == 0 || it == patterns.end()) {
        return;
    }
    auto& uses = it->second.uses;
    uses = delta < 0 ? (uses > 0 ? uses - 1 : 0) : uses + 1;
}

std::string nameOf(const std::map<std::uint16_t, stringmap::Entry>& patterns, std::uint16_t id) {
    const auto it = patterns.find(id);
    return it != patterns.end() ? it->second.name : std::string();
}

} // namespace

std::string floorPatternName(std::uint32_t guid) { return std::format("{:08x}", guid); }

std::uint16_t patternId(std::map<std::uint16_t, stringmap::Entry>& patterns, const std::string& name) {
    // Lots may spell a GUID in either case.
    const auto same = [](const std::string& a, const std::string& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
            const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
            return lower(x) == lower(y);
        });
    };
    for (const auto& [id, entry] : patterns) {
        if (same(entry.name, name)) {
            return id;
        }
    }
    for (std::uint32_t id = 1; id <= std::numeric_limits<std::uint16_t>::max(); ++id) {
        if (!patterns.contains(static_cast<std::uint16_t>(id))) {
            patterns[static_cast<std::uint16_t>(id)] = {name, 0};
            return static_cast<std::uint16_t>(id);
        }
    }
    return 0;
}

FloorChange layFloor(array3d::Array3D& floors, std::map<std::uint16_t, stringmap::Entry>& patterns, std::int32_t level, TileRect rect,
                     const std::string& name, bool anywhere, const array3d::Array3D* surface) {
    FloorChange change;
    const std::uint16_t id = patternId(patterns, name);
    if (id == 0) {
        return change;
    }
    eachTile(floors, level, rect, [&](std::int32_t x, std::int32_t y) {
        for (std::size_t q = 0; q < kQuarters; ++q) {
            const std::uint16_t old = quarterAt(floors, level, x, y, q);
            if (old == id || (old == 0 && (!anywhere || !hasSurface(surface, level, x, y, q)))) {
                continue;
            }
            if (old != 0) {
                ++change.replaced[nameOf(patterns, old)];
                countUse(patterns, old, -1);
            }
            setQuarter(floors, level, x, y, q, id);
            countUse(patterns, id, +1);
            ++change.quarters;
        }
    });
    return change;
}

FloorChange removeFloor(array3d::Array3D& floors, std::map<std::uint16_t, stringmap::Entry>& patterns, std::int32_t level, TileRect rect) {
    FloorChange change;
    eachTile(floors, level, rect, [&](std::int32_t x, std::int32_t y) {
        for (std::size_t q = 0; q < kQuarters; ++q) {
            const std::uint16_t old = quarterAt(floors, level, x, y, q);
            if (old == 0) {
                continue;
            }
            ++change.replaced[nameOf(patterns, old)];
            countUse(patterns, old, -1);
            setQuarter(floors, level, x, y, q, 0);
            ++change.quarters;
        }
    });
    return change;
}

std::int32_t floorPrice(std::int32_t pricePerTile, std::size_t quarters) {
    const auto total = static_cast<std::int64_t>(pricePerTile) * static_cast<std::int64_t>(quarters);
    const auto price = (total + static_cast<std::int64_t>(kQuarters) - 1) / static_cast<std::int64_t>(kQuarters);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(price, 0, std::numeric_limits<std::int32_t>::max()));
}

} // namespace ts2::content
