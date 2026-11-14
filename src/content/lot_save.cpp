#include "content/lot_save.hpp"

#include "formats/binary_reader.hpp"
#include "formats/fami.hpp"
#include "formats/resource_types.hpp"
#include "formats/simi.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>

namespace ts2::content {

namespace {

constexpr std::uint32_t kRoomGraph = 5, kWallGraph = 24;
// SIMI: a 64-byte name, "IMIS", the version and a reserved word, then the globals as i16s. The
// budget is kept as funds % 10000 in global 0x0D and funds / 10000 in global 0x0E.
constexpr std::size_t kFirstGlobal = 76;
constexpr std::size_t kBudgetLow = 0x0D, kBudgetHigh = 0x0E;
// The bytes of a saved resource before its name ends: the name, then the type id and version.
constexpr std::size_t kHeader = 72;

void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) {
        out.push_back(static_cast<std::uint8_t>(v >> s));
    }
}

void set16(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t v) {
    out.at(at) = static_cast<std::uint8_t>(v);
    out.at(at + 1) = static_cast<std::uint8_t>(v >> 8);
}

void set32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t v) {
    for (std::size_t k = 0; k < 4; ++k) {
        out.at(at + k) = static_cast<std::uint8_t>(v >> (8 * k));
    }
}

void putVarString(std::vector<std::uint8_t>& out, const std::string& text) {
    std::size_t length = text.size();
    do {
        const auto low = static_cast<std::uint8_t>(length & 0x7F);
        length >>= 7;
        out.push_back(length != 0 ? static_cast<std::uint8_t>(low | 0x80) : low);
    } while (length != 0);
    out.insert(out.end(), text.begin(), text.end());
}

void putShortString(std::vector<std::uint8_t>& out, const std::string& text) {
    if (text.size() > 255) {
        throw std::runtime_error("name too long to save: " + text);
    }
    out.push_back(static_cast<std::uint8_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

// A grid saved as its header and then its elements: the header as saved, the elements edited.
std::vector<std::uint8_t> withElements(const std::vector<std::uint8_t>& saved, std::size_t savedElements,
                                       const std::vector<std::uint8_t>& elements) {
    if (savedElements != elements.size() || savedElements > saved.size()) {
        throw std::runtime_error("the grid's size changed");
    }
    std::vector<std::uint8_t> out(saved.begin(), saved.end() - static_cast<std::ptrdiff_t>(savedElements));
    out.insert(out.end(), elements.begin(), elements.end());
    return out;
}

std::vector<std::uint8_t> patternMap(const std::vector<std::uint8_t>& saved, const std::map<std::uint16_t, stringmap::Entry>& patterns) {
    BinaryReader r(saved);
    r.skip(kHeader);
    r.varString();  // "cStringMap"
    std::vector<std::uint8_t> out(saved.begin(), saved.begin() + static_cast<std::ptrdiff_t>(r.position()));
    put32(out, static_cast<std::uint32_t>(patterns.size()));
    for (const auto& [id, e] : patterns) {
        putVarString(out, e.name);
        put16(out, id);
        put32(out, e.uses);
    }
    return out;
}

std::vector<std::uint8_t> wallLayer(const std::vector<std::uint8_t>& saved, const std::vector<walllayer::Entry>& entries) {
    BinaryReader r(saved);
    r.skip(kHeader);
    r.skip(r.u8());  // "cWallLayer"
    std::vector<std::uint8_t> out(saved.begin(), saved.begin() + static_cast<std::ptrdiff_t>(r.position()));
    put32(out, static_cast<std::uint32_t>(entries.size()));
    for (const auto& e : entries) {
        put32(out, e.wall);
        put32(out, e.type);
        put16(out, e.left);
        put16(out, e.right);
    }
    return out;
}

// The terrain textures with the edited paint list; whatever the game saved after the list is kept.
std::vector<std::uint8_t> lotTexture(const std::vector<std::uint8_t>& saved, const terrain::LotTexture& textures) {
    BinaryReader r(saved);
    r.skip(kHeader + 8);  // and the width and height
    r.skip(r.u8());       // the canvas
    for (std::uint32_t i = 0, count = r.u32(); i < count; ++i) {
        r.skip(r.u8());
    }
    const std::size_t end = r.position();
    std::vector<std::uint8_t> out(saved.begin(), saved.begin() + static_cast<std::ptrdiff_t>(kHeader));
    put32(out, textures.width);
    put32(out, textures.height);
    putShortString(out, textures.canvas);
    put32(out, static_cast<std::uint32_t>(textures.paints.size()));
    for (const auto& paint : textures.paints) {
        putShortString(out, paint);
    }
    out.insert(out.end(), saved.begin() + static_cast<std::ptrdiff_t>(end), saved.end());
    return out;
}

bool sameCovering(const walllayer::Entry& a, const walllayer::Entry& b) {
    return a.wall == b.wall && a.type == b.type && a.left == b.left && a.right == b.right;
}

struct Saved {
    const dbpf::IndexEntry* entry = nullptr;
    std::vector<std::uint8_t> bytes;
};

} // namespace

LotChanges lotChanges(dbpf::Package& original, const Lot& edited, const LotSaveParts& parts) {
    LotChanges changes;
    std::map<std::pair<std::uint32_t, std::uint32_t>, Saved> saved;
    auto want = [&](std::uint32_t type, std::uint32_t instance) {
        if (type == types::kArray3D) return parts.floors && instance == kFloorGridInstance;
        if (type == types::kPatternMap) {
            return (parts.floors && instance == kFloorPatternMapInstance) || (parts.wallCoverings && instance == kWallPatternMapInstance);
        }
        if (type == types::kWallLayer) return parts.wallCoverings && instance == kWallCoveringLayerInstance;
        if (type == types::kWallGraph) return parts.wallCoverings && (instance == kRoomGraph || instance == kWallGraph);
        if (type == types::kLotTexture || type == types::kArray2D) return parts.terrainPaint && (type == types::kLotTexture || instance >= kFirstPaintMapInstance);
        if (type == simi::kType) return parts.fundsChange != 0;
        return false;
    };
    for (const auto& e : original.entries()) {
        if (want(e.key.type, e.key.instance) && !saved.contains({e.key.type, e.key.instance})) {
            saved[{e.key.type, e.key.instance}] = {&e, original.read(e)};
        }
    }
    auto find = [&](std::uint32_t type, std::uint32_t instance) -> Saved* {
        const auto it = saved.find({type, instance});
        return it != saved.end() ? &it->second : nullptr;
    };
    auto put = [&](const Saved& s, std::vector<std::uint8_t> bytes, std::uint32_t instance) {
        if (instance == s.entry->key.instance && bytes == s.bytes) {
            return;
        }
        auto key = s.entry->key;
        key.instance = instance;
        changes.resources.push_back({key, std::move(bytes)});
    };
    auto attempt = [&](const std::string& what, auto&& save) {
        try {
            save();
        } catch (const std::exception& ex) {
            changes.problems.push_back(std::format("{}: {}", what, ex.what()));
        }
    };

    if (parts.floors) {
        attempt("floors", [&] {
            const auto* grid = find(types::kArray3D, kFloorGridInstance);
            const auto edits = edited.grids.find(kFloorGridInstance);
            const auto* patterns = find(types::kPatternMap, kFloorPatternMapInstance);
            if (grid == nullptr || patterns == nullptr || edits == edited.grids.end()) {
                throw std::runtime_error("the lot has no floor grid or floor pattern map");
            }
            const auto before = array3d::parse(grid->bytes);
            put(*grid, withElements(grid->bytes, before.elements.size(), edits->second.elements), kFloorGridInstance);
            put(*patterns, patternMap(patterns->bytes, edited.floorPatterns), kFloorPatternMapInstance);
        });
    }

    if (parts.wallCoverings) {
        attempt("wall coverings", [&] {
            const auto* layer = find(types::kWallLayer, kWallCoveringLayerInstance);
            const auto* patterns = find(types::kPatternMap, kWallPatternMapInstance);
            const auto* rooms = find(types::kWallGraph, kRoomGraph);
            const auto* walls = find(types::kWallGraph, kWallGraph);
            if (layer == nullptr || patterns == nullptr || rooms == nullptr || walls == nullptr) {
                throw std::runtime_error("the lot has no wall layer, wall pattern map or wall graphs");
            }
            const auto roomGraph = wallgraph::parse(rooms->bytes);
            const auto wallGraph = wallgraph::parse(walls->bytes);
            const auto savedLayer = walllayer::parse(layer->bytes);
            // Only the room walls whose coverings were changed are written back, so walls the
            // loading couldn't match keep theirs.
            const auto loaded = coveringsOfRoomWalls(roomGraph, wallGraph, savedLayer);
            std::map<std::uint32_t, const walllayer::Entry*> before;
            for (const auto& c : loaded) {
                before.emplace(c.wall, &c);
            }
            std::vector<walllayer::Entry> changed;
            for (const auto& c : edited.wallCoverings) {
                const auto it = before.find(c.wall);
                if (it == before.end() || !sameCovering(*it->second, c)) {
                    changed.push_back(c);
                }
            }
            put(*layer, wallLayer(layer->bytes, coveringsOfWallGraph(roomGraph, wallGraph, savedLayer, changed)), kWallCoveringLayerInstance);
            put(*patterns, patternMap(patterns->bytes, edited.wallPatterns), kWallPatternMapInstance);
        });
    }

    if (parts.terrainPaint) {
        attempt("terrain paint", [&] {
            const auto* textures = find(types::kLotTexture, 0);
            if (textures == nullptr) {
                for (auto& [k, s] : saved) {
                    if (k.first == types::kLotTexture) {
                        textures = &s;
                        break;
                    }
                }
            }
            if (textures == nullptr || !edited.terrainTextures) {
                throw std::runtime_error("the lot has no terrain textures");
            }
            const auto* first = find(types::kArray2D, kFirstPaintMapInstance);
            for (std::size_t n = 0; n < edited.terrainTextures->paints.size(); ++n) {
                const auto instance = kFirstPaintMapInstance + static_cast<std::uint32_t>(n);
                const auto edits = edited.flatGrids.find(instance);
                if (edits == edited.flatGrids.end()) {
                    throw std::runtime_error(std::format("paint {} has no strength map", n));
                }
                const Saved* map = find(types::kArray2D, instance);
                const Saved* header = map != nullptr ? map : first;  // a new paint's map takes the first's header
                if (header == nullptr) {
                    throw std::runtime_error("the lot has no paint strength maps to follow");
                }
                const auto before = terrain::parseArray2D(header->bytes);
                if (before.width != edits->second.width || before.height != edits->second.height) {
                    throw std::runtime_error(std::format("paint {}'s map isn't the lot's size", n));
                }
                put(*header, withElements(header->bytes, before.elements.size(), edits->second.elements), instance);
            }
            put(*textures, lotTexture(textures->bytes, *edited.terrainTextures), textures->entry->key.instance);
        });
    }

    if (parts.fundsChange != 0) {
        attempt("household funds", [&] {
            const Saved* state = nullptr;
            for (auto& [k, s] : saved) {
                if (k.first == simi::kType) {
                    state = &s;
                    break;
                }
            }
            const auto globals = state != nullptr ? simi::parse(state->bytes).globals : std::vector<std::int16_t>{};
            if (globals.size() != simi::kGlobalCount || state->bytes.size() < kFirstGlobal + 2 * (kBudgetHigh + 1)) {
                throw std::runtime_error("the lot has no simulator state");
            }
            const std::int32_t was = static_cast<std::int32_t>(globals[kBudgetHigh]) * 10000 + globals[kBudgetLow];
            const std::int32_t funds = std::max<std::int32_t>(was + parts.fundsChange, 0);
            changes.funds = funds;
            auto bytes = state->bytes;
            set16(bytes, kFirstGlobal + 2 * kBudgetLow, static_cast<std::uint16_t>(funds % 10000));
            set16(bytes, kFirstGlobal + 2 * kBudgetHigh, static_cast<std::uint16_t>(funds / 10000));
            put(*state, std::move(bytes), state->entry->key.instance);
        });
    }
    return changes;
}

std::vector<ReplacedResource> familyFundsChanges(dbpf::Package& hood, std::int32_t lotNumber, std::int32_t funds) {
    std::vector<ReplacedResource> out;
    if (lotNumber <= 0) {
        return out;
    }
    for (const auto& e : hood.entries()) {
        if (e.key.type != types::kFamily) {
            continue;
        }
        auto bytes = hood.read(e);
        fami::Family family;
        try {
            family = fami::parse(bytes);
        } catch (const std::exception&) {
            continue;
        }
        if (family.lot != lotNumber) {
            continue;
        }
        // magic, version, unknown0 and lot, then from 0x50 the current lot, from 0x55 the vacation
        // lot, then value4 and the funds.
        const std::size_t at = 16 + (family.version >= 0x50 ? 4 : 0) + (family.version >= 0x55 ? 4 : 0) + 4;
        set32(bytes, at, static_cast<std::uint32_t>(funds));
        if (family.version >= 0x51) {
            set32(bytes, bytes.size() - family.extraBytes - 4, static_cast<std::uint32_t>(funds));
        }
        if (family.funds != funds || family.funds2 != funds) {
            out.push_back({e.key, std::move(bytes)});
        }
    }
    return out;
}

} // namespace ts2::content
