#include "formats/nhood_objects.hpp"

#include "formats/binary_reader.hpp"

#include <bit>
#include <stdexcept>
#include <string>

namespace ts2::nhood {

namespace {

constexpr std::size_t kRoadBytes = 124;
constexpr std::size_t kBridgeSpanBytes = 40;
// Plants and decorations saved before version 3 used a smaller tile.
constexpr float kOldTileScale = 1.25f;

// How a list of plants or decorations is laid out. From list version `newFrom` on, each record
// starts with its placement, position and bounds and its own version, before that with its position
// alone. `oldBytes` and `newBytes` are a record's fewest bytes in each layout.
struct PlacedLayout {
    std::uint16_t newFrom;
    std::size_t oldBytes, newBytes;
    bool oldHasBounds;  // whether a record older than `newFrom` keeps the bounds it read
};
constexpr PlacedLayout kPlantLayout{6, 12, 30, true};
constexpr PlacedLayout kDecorationLayout{7, 16, 34, false};

// The list's record count, refused when the data can't hold that many records.
std::uint32_t placedCount(BinaryReader& r, std::uint16_t listVersion, const PlacedLayout& layout, const char* what) {
    const std::uint32_t count = r.u32();
    if (count > r.remaining() / (listVersion < layout.newFrom ? layout.oldBytes : layout.newBytes)) {
        throw std::runtime_error(std::string("neighborhood objects: bad ") + what + " count");
    }
    return count;
}

// Reads a record's start and returns its version (the list's in the old layout).
std::uint16_t placedHeader(BinaryReader& r, std::uint16_t listVersion, const PlacedLayout& layout, PlacedObject& f) {
    if (listVersion < layout.newFrom) {
        for (auto& v : f.position) {
            v = r.f32();
        }
        return listVersion;
    }
    const std::uint8_t placement = r.u8();
    for (auto& v : f.position) {
        v = r.f32();
    }
    for (auto& v : f.bounds) {
        v = r.f32();
    }
    if (placement < 2) {
        f.bounds = {};
    }
    return r.u8();
}

// Reads what an old record keeps after its fields, and scales the oldest records' positions.
void placedTail(BinaryReader& r, std::uint16_t version, const PlacedLayout& layout, PlacedObject& f) {
    if (version < 3) {
        f.position[0] *= kOldTileScale;
        f.position[1] *= kOldTileScale;
    }
    if (version < layout.newFrom) {
        if (version > 3) {
            r.skip(16);
        }
        if (!layout.oldHasBounds) {
            f.bounds = {};
        }
    }
}

RoadTile roadTile(BinaryReader& r) {
    RoadTile road;
    r.u8();  // record version
    for (auto& v : road.center) {
        v = r.f32();
    }
    for (auto& v : road.bounds) {
        v = r.f32();
    }
    r.u8();
    for (auto& corner : road.corners) {
        for (auto& v : corner) {
            v = r.f32();
        }
    }
    road.piece = r.u32();
    r.skip(10);  // the piece's connections and flags (not needed to draw it)
    return road;
}

} // namespace

Objects parseObjects(std::span<const std::uint8_t> data, ObjectsLayout* layout) {
    ObjectsLayout spans;
    BinaryReader r(data);
    Objects out;
    out.version = r.u32();
    // Plants: the angle, then the catalog guid, from the record versions that have them.
    spans.flora.start = r.position();
    const std::uint16_t plantVersion = r.u16();
    spans.flora.listVersion = plantVersion;
    out.flora.resize(placedCount(r, plantVersion, kPlantLayout, "plant"));
    for (auto& f : out.flora) {
        const std::size_t first = r.position();
        const std::uint16_t version = placedHeader(r, plantVersion, kPlantLayout, f);
        if (version >= 7) {
            f.angle = r.f32();
        }
        if (version >= 8) {
            f.type = r.u32();
        }
        placedTail(r, version, kPlantLayout, f);
        spans.flora.records.emplace_back(first, r.position());
    }
    spans.flora.end = r.position();
    // Roads.
    r.u16();
    const std::uint32_t roads = r.u32();
    if (roads > r.remaining() / kRoadBytes) {
        throw std::runtime_error("neighborhood objects: bad road count");
    }
    out.roads.resize(roads);
    for (auto& road : out.roads) {
        road = roadTile(r);
    }
    // Bridges: a road record, a record version from list version 3 on, then a point, the piece's
    // turn and another point. What the two points are for is not known yet.
    const std::uint16_t bridgeVersion = r.u16();
    const std::uint32_t bridges = r.u32();
    const std::size_t bridgeBytes = kRoadBytes + (bridgeVersion > 2 ? 1 : 0) + kBridgeSpanBytes;
    if (bridges > r.remaining() / bridgeBytes) {
        throw std::runtime_error("neighborhood objects: bad bridge count");
    }
    out.bridges.resize(bridges);
    for (auto& bridge : out.bridges) {
        bridge.road = roadTile(r);
        if (bridgeVersion > 2) {
            r.u8();
        }
        r.skip(12);
        for (auto& v : bridge.rotation) {
            v = r.f32();
        }
        r.skip(12);
    }
    // Decorations: the catalog guid from version 8, then the angle in every version.
    spans.decorations.start = r.position();
    const std::uint16_t decorVersion = r.u16();
    spans.decorations.listVersion = decorVersion;
    out.decorations.resize(placedCount(r, decorVersion, kDecorationLayout, "decoration"));
    for (auto& f : out.decorations) {
        const std::size_t first = r.position();
        const std::uint16_t version = placedHeader(r, decorVersion, kDecorationLayout, f);
        if (version >= 8) {
            f.type = r.u32();
        }
        f.angle = r.f32();
        placedTail(r, version, kDecorationLayout, f);
        spans.decorations.records.emplace_back(first, r.position());
    }
    spans.decorations.end = r.position();
    if (layout != nullptr) {
        *layout = std::move(spans);
    }
    return out;
}

namespace {

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}
void putFloat(std::vector<std::uint8_t>& out, float v) { put32(out, std::bit_cast<std::uint32_t>(v)); }

// A list written again: its version, its count, the records kept as they were, then the new ones.
void writeList(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> data, const PlacedSpans& list, const std::vector<bool>& keep,
               const std::vector<PlacedObject>& add, const PlacedLayout& layout, bool decorations) {
    if (!add.empty() && list.listVersion < layout.newFrom) {
        throw std::runtime_error(std::string("neighborhood objects: the ") + (decorations ? "decoration" : "plant") +
                                 " list is laid out as an old version of the game saved it, so nothing can be added to it");
    }
    std::vector<std::pair<std::size_t, std::size_t>> kept;
    for (std::size_t i = 0; i < list.records.size(); ++i) {
        if (i >= keep.size() || keep[i]) {
            kept.push_back(list.records[i]);
        }
    }
    out.push_back(static_cast<std::uint8_t>(list.listVersion));
    out.push_back(static_cast<std::uint8_t>(list.listVersion >> 8));
    put32(out, static_cast<std::uint32_t>(kept.size() + add.size()));
    for (const auto& [first, end] : kept) {
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(first), data.begin() + static_cast<std::ptrdiff_t>(end));
    }
    for (const auto& item : add) {
        out.push_back(2);  // placed, with its bounds
        for (const float v : item.position) {
            putFloat(out, v);
        }
        for (const float v : item.bounds) {
            putFloat(out, v);
        }
        out.push_back(8);  // record version
        // A plant's angle comes before its type, a decoration's after.
        if (decorations) {
            put32(out, item.type);
            putFloat(out, item.angle);
        } else {
            putFloat(out, item.angle);
            put32(out, item.type);
        }
    }
}

} // namespace

std::vector<std::uint8_t> rewritePlaced(std::span<const std::uint8_t> data, const ObjectsLayout& layout, const std::vector<bool>& keepFlora,
                                        const std::vector<PlacedObject>& addFlora, const std::vector<bool>& keepDecorations,
                                        const std::vector<PlacedObject>& addDecorations) {
    const auto& f = layout.flora;
    const auto& d = layout.decorations;
    if (!(f.start <= f.end && f.end <= d.start && d.start <= d.end && d.end <= data.size())) {
        throw std::runtime_error("neighborhood objects: the lists don't lie where they were read");
    }
    std::vector<std::uint8_t> out(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(f.start));
    writeList(out, data, f, keepFlora, addFlora, kPlantLayout, false);
    out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(f.end), data.begin() + static_cast<std::ptrdiff_t>(d.start));
    writeList(out, data, d, keepDecorations, addDecorations, kDecorationLayout, true);
    out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(d.end), data.end());
    return out;
}

} // namespace ts2::nhood
