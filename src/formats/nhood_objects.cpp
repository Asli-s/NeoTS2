#include "formats/nhood_objects.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

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

Objects parseObjects(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Objects out;
    out.version = r.u32();
    // Plants: the angle, then the catalog guid, from the record versions that have them.
    const std::uint16_t plantVersion = r.u16();
    out.flora.resize(placedCount(r, plantVersion, kPlantLayout, "plant"));
    for (auto& f : out.flora) {
        const std::uint16_t version = placedHeader(r, plantVersion, kPlantLayout, f);
        if (version >= 7) {
            f.angle = r.f32();
        }
        if (version >= 8) {
            f.type = r.u32();
        }
        placedTail(r, version, kPlantLayout, f);
    }
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
    const std::uint16_t decorVersion = r.u16();
    out.decorations.resize(placedCount(r, decorVersion, kDecorationLayout, "decoration"));
    for (auto& f : out.decorations) {
        const std::uint16_t version = placedHeader(r, decorVersion, kDecorationLayout, f);
        if (version >= 8) {
            f.type = r.u32();
        }
        f.angle = r.f32();
        placedTail(r, version, kDecorationLayout, f);
    }
    return out;
}

namespace {

constexpr std::uint8_t kTileVersion = 2;
constexpr std::uint8_t kPlacedVersion = 8, kRoadVersion = 3, kBridgeVersion = 3;
constexpr std::uint16_t kPlacedListVersion = 8, kRoadListVersion = 3, kBridgeListVersion = 3;
// The fewest bytes of each entry: a tile, then a plant or decoration, a road, a road part.
constexpr std::size_t kTileBytes = 29, kPlacedBytes = kTileBytes + 9, kRoadRecordBytes = kTileBytes + 95, kRoadPartBytes = 64;

// Any part that does not match what the game writes now.
struct NotWritten {};

template <std::size_t N>
void readFloats(BinaryReader& r, std::array<float, N>& out) {
    for (float& v : out) {
        v = r.f32();
    }
}

template <std::size_t N>
void writeFloats(BinaryWriter& w, const std::array<float, N>& values) {
    for (const float v : values) {
        w.f32(v);
    }
}

void expect8(BinaryReader& r, std::uint8_t value) {
    if (r.u8() != value) {
        throw NotWritten{};
    }
}

std::size_t listCount(BinaryReader& r, std::uint16_t version, std::size_t entryBytes) {
    if (r.u16() != version) {
        throw NotWritten{};
    }
    return r.u32Count(entryBytes);
}

void readTile(BinaryReader& r, std::array<float, 3>& center, std::array<float, 4>& bounds) {
    expect8(r, kTileVersion);
    readFloats(r, center);
    readFloats(r, bounds);
}

void writeTile(BinaryWriter& w, const std::array<float, 3>& center, const std::array<float, 4>& bounds) {
    w.u8(kTileVersion);
    writeFloats(w, center);
    writeFloats(w, bounds);
}

RoadRecord readRoad(BinaryReader& r) {
    RoadRecord road;
    readTile(r, road.center, road.bounds);
    expect8(r, kRoadVersion);
    for (auto& corner : road.corners) {
        readFloats(r, corner);
    }
    road.piece = r.u32();
    road.value = r.u32();
    road.flags1 = r.u8();
    road.flags2 = r.u8();
    road.parts.resize(r.u32Count(kRoadPartBytes));
    for (RoadPart& part : road.parts) {
        part.value = r.u32();
        for (auto& corner : part.corners) {
            readFloats(r, corner);
        }
    }
    return road;
}

void writeRoad(BinaryWriter& w, const RoadRecord& road) {
    writeTile(w, road.center, road.bounds);
    w.u8(kRoadVersion);
    for (const auto& corner : road.corners) {
        writeFloats(w, corner);
    }
    w.u32(road.piece);
    w.u32(road.value);
    w.u8(road.flags1);
    w.u8(road.flags2);
    w.count(road.parts.size());
    for (const RoadPart& part : road.parts) {
        w.u32(part.value);
        for (const auto& corner : part.corners) {
            writeFloats(w, corner);
        }
    }
}

} // namespace

std::optional<ObjectsRecord> readObjectsRecord(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        if (r.u32() != kObjectsWrittenVersion) {
            return std::nullopt;
        }
        ObjectsRecord out;
        out.flora.resize(listCount(r, kPlacedListVersion, kPlacedBytes));
        for (PlacedRecord& f : out.flora) {
            readTile(r, f.position, f.bounds);
            expect8(r, kPlacedVersion);
            f.angle = r.f32();
            f.type = r.u32();
        }
        out.roads.resize(listCount(r, kRoadListVersion, kRoadRecordBytes));
        for (RoadRecord& road : out.roads) {
            road = readRoad(r);
        }
        out.bridges.resize(listCount(r, kBridgeListVersion, kRoadRecordBytes + 41));
        for (BridgeRecord& bridge : out.bridges) {
            bridge.road = readRoad(r);
            expect8(r, kBridgeVersion);
            readFloats(r, bridge.start);
            readFloats(r, bridge.rotation);
            readFloats(r, bridge.end);
        }
        out.decorations.resize(listCount(r, kPlacedListVersion, kPlacedBytes));
        for (PlacedRecord& f : out.decorations) {
            readTile(r, f.position, f.bounds);
            expect8(r, kPlacedVersion);
            f.type = r.u32();
            f.angle = r.f32();
        }
        if (r.remaining() != 0) {
            return std::nullopt;
        }
        return out;
    } catch (const NotWritten&) {
        return std::nullopt;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeObjectsRecord(const ObjectsRecord& record) {
    BinaryWriter w;
    w.u32(kObjectsWrittenVersion);
    w.u16(kPlacedListVersion);
    w.count(record.flora.size());
    for (const PlacedRecord& f : record.flora) {
        writeTile(w, f.position, f.bounds);
        w.u8(kPlacedVersion);
        w.f32(f.angle);
        w.u32(f.type);
    }
    w.u16(kRoadListVersion);
    w.count(record.roads.size());
    for (const RoadRecord& road : record.roads) {
        writeRoad(w, road);
    }
    w.u16(kBridgeListVersion);
    w.count(record.bridges.size());
    for (const BridgeRecord& bridge : record.bridges) {
        writeRoad(w, bridge.road);
        w.u8(kBridgeVersion);
        writeFloats(w, bridge.start);
        writeFloats(w, bridge.rotation);
        writeFloats(w, bridge.end);
    }
    w.u16(kPlacedListVersion);
    w.count(record.decorations.size());
    for (const PlacedRecord& f : record.decorations) {
        writeTile(w, f.position, f.bounds);
        w.u8(kPlacedVersion);
        w.u32(f.type);
        w.f32(f.angle);
    }
    return w.take();
}

} // namespace ts2::nhood
