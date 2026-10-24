#include "content/lot.hpp"

#include "formats/cobject.hpp"
#include "formats/dbpf.hpp"
#include "formats/resource_types.hpp"
#include "formats/scenegraph.hpp"
#include "formats/simi.hpp"
#include "formats/srel.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace ts2::content {

namespace {

constexpr std::size_t kObjectIdField = 0x0B;
constexpr std::uint32_t kRoomGraphInstance = 5;
// The graph of the lot's actual walls and fences. The wall covering layer names its walls.
constexpr std::uint32_t kWallGraphInstance = 24;

xobj::RecordKind recordKind(std::uint32_t resourceType) {
    return resourceType == types::kSavedPerson               ? xobj::RecordKind::Person
           : resourceType == types::kSavedObjectWithMaterial ? xobj::RecordKind::ObjectWithMaterial
                                                             : xobj::RecordKind::Object;
}

// A wall's place: its level and its two ends in half tiles, the lower end first.
using WallPlace = std::tuple<std::int32_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t>;

std::optional<WallPlace> placeOf(const wallgraph::Wall& wall,
                                 const std::unordered_map<std::int32_t, const wallgraph::Point*>& points) {
    const auto a = points.find(wall.from), b = points.find(wall.to);
    if (a == points.end() || b == points.end()) {
        return std::nullopt;
    }
    auto halves = [](float v) -> std::optional<std::int64_t> {
        if (!std::isfinite(v) || std::fabs(v) > 1e6f) {
            return std::nullopt;
        }
        return std::llround(static_cast<double>(v) * 2);
    };
    const auto ax = halves(a->second->x), ay = halves(a->second->y), bx = halves(b->second->x), by = halves(b->second->y);
    if (!ax || !ay || !bx || !by) {
        return std::nullopt;
    }
    const WallPlace forward{a->second->level, *ax, *ay, *bx, *by}, backward{a->second->level, *bx, *by, *ax, *ay};
    return std::min(forward, backward);
}

std::unordered_map<std::int32_t, const wallgraph::Point*> pointsOf(const wallgraph::WallGraph& graph) {
    std::unordered_map<std::int32_t, const wallgraph::Point*> points;
    for (const auto& p : graph.points) {
        points.emplace(p.id, &p);
    }
    return points;
}

} // namespace

std::vector<walllayer::Entry> coveringsOfRoomWalls(const wallgraph::WallGraph& rooms, const wallgraph::WallGraph& walls,
                                                   const std::vector<walllayer::Entry>& coverings) {
    std::unordered_map<std::int32_t, const walllayer::Entry*> byWall;
    for (const auto& c : coverings) {
        byWall.emplace(static_cast<std::int32_t>(c.wall), &c);
    }
    const auto wallPoints = pointsOf(walls);
    std::map<WallPlace, const walllayer::Entry*> byPlace;
    for (const auto& w : walls.walls) {
        const auto place = placeOf(w, wallPoints);
        const auto covering = byWall.find(w.id);
        if (place && covering != byWall.end()) {
            byPlace.emplace(*place, covering->second);
        }
    }
    const auto roomPoints = pointsOf(rooms);
    std::vector<walllayer::Entry> out;
    for (const auto& w : rooms.walls) {
        const auto place = placeOf(w, roomPoints);
        const auto covering = place ? byPlace.find(*place) : byPlace.end();
        if (covering != byPlace.end()) {
            walllayer::Entry e = *covering->second;
            e.wall = static_cast<std::uint32_t>(w.id);
            out.push_back(e);
        }
    }
    return out;
}

Lot loadLot(const std::filesystem::path& path) {
    dbpf::Package package{path};
    Lot lot;
    std::optional<wallgraph::WallGraph> wallGraph;
    bool haveTypes = false;
    bool haveMap = false;
    std::map<std::uint32_t, std::vector<const dbpf::IndexEntry*>> records;
    for (const auto& e : package.entries()) {
        if (e.key.type == types::kObjectTypeTable) {
            lot.types = objt::parse(package.read(e));
            haveTypes = true;
        } else if (e.key.type == types::kObjectMap && e.key.instance == 1) {
            lot.objectMap = objm::parse(package.read(e));
            haveMap = true;
        } else if (e.key.type == types::kObjectMap && e.key.instance == 2) {
            try {
                lot.objectGroups = objm::parseGroups(package.read(e));
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("object groups: ") + ex.what()});
            }
        } else if (e.key.type == types::kWallGraph && e.key.instance == kRoomGraphInstance) {
            lot.roomGraph = wallgraph::parse(package.read(e));
        } else if (e.key.type == types::kWallGraph && e.key.instance == kWallGraphInstance) {
            try {
                wallGraph = wallgraph::parse(package.read(e));
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("wall graph: ") + ex.what()});
            }
        } else if (e.key.type == types::kWallGraph &&
                   std::ranges::find(kLayerGraphInstances, e.key.instance) != std::end(kLayerGraphInstances)) {
            lot.layerGraphs.insert_or_assign(e.key.instance, wallgraph::parse(package.read(e)));
        } else if (e.key.type == types::kSceneObject) {
            try {
                const auto data = package.read(e);
                lot.objectModels[e.key.instance] = scenegraph::parseSceneObject(data).model;
                try {
                    auto saved = cobject::parse(data);
                    if (!saved.animations.empty()) {
                        lot.savedAnimations.emplace(e.key.instance, std::move(saved.animations));
                    }
                    if (!saved.joints.empty()) {
                        lot.savedJoints.emplace(e.key.instance, std::move(saved.joints));
                    }
                } catch (const std::exception& ex) {
                    lot.problems.push_back({e.key.instance, std::string("saved animations: ") + ex.what()});
                }
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("scene object: ") + ex.what()});
            }
        } else if (e.key.type == types::kSimRelationship) {
            const auto from = static_cast<std::int16_t>(e.key.instance >> 16);
            const auto to = static_cast<std::int16_t>(e.key.instance & 0xFFFF);
            try {
                lot.relationships[{from, to}] = srel::parse(package.read(e)).values;
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("relationship: ") + ex.what()});
            }
        } else if (e.key.type == types::kDataTables) {
            try {
                lot.dataTables.emplace(e.key.instance, datatables::parse(package.read(e)));
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("data tables: ") + ex.what()});
            }
        } else if (e.key.type == types::kArray3D) {
            try {
                if (e.key.instance == kListGridInstance) {
                    lot.listGrids.emplace(e.key.instance, array3d::parseLists(package.read(e)));
                } else {
                    lot.grids.emplace(e.key.instance, array3d::parse(package.read(e)));
                }
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("grid: ") + ex.what()});
            }
        } else if (e.key.type == types::kArray2D) {
            try {
                lot.flatGrids.emplace(e.key.instance, e.key.instance == kByteListGridInstance
                                                          ? terrain::parseByteLists(package.read(e))
                                                          : terrain::parseArray2D(package.read(e)));
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("flat grid: ") + ex.what()});
            }
        } else if (e.key.type == types::kLotTexture) {
            try {
                lot.terrainTextures = terrain::parseLotTexture(package.read(e));
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("terrain textures: ") + ex.what()});
            }
        } else if (e.key.type == types::kPatternMap &&
                   (e.key.instance == kFloorPatternMapInstance || e.key.instance == kWallPatternMapInstance)) {
            try {
                (e.key.instance == kFloorPatternMapInstance ? lot.floorPatterns : lot.wallPatterns) =
                    stringmap::parse(package.read(e));
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("pattern map: ") + ex.what()});
            }
        } else if (e.key.type == types::kWallLayer && e.key.instance == kWallCoveringLayerInstance) {
            try {
                lot.wallCoverings = walllayer::parse(package.read(e));
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("wall layer: ") + ex.what()});
            }
        } else if (e.key.type == types::kFencePostLayer) {
            try {
                lot.fencePosts = fencepostlayer::parse(package.read(e));
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("fence posts: ") + ex.what()});
            }
        } else if (e.key.type == simi::kType) {
            try {
                lot.globals = simi::parse(package.read(e)).globals;
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("simulator state: ") + ex.what()});
            }
        } else if (e.key.type == types::kRoofs) {
            try {
                lot.roofs = roofs::parse(package.read(e));
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("roofs: ") + ex.what()});
            }
        } else if (e.key.type == types::kSavedObject || e.key.type == types::kSavedObjectWithMaterial ||
                   e.key.type == types::kSavedPerson) {
            records[e.key.instance].push_back(&e);
        }
    }
    if (lot.roomGraph && wallGraph) {
        lot.wallCoverings = coveringsOfRoomWalls(*lot.roomGraph, *wallGraph, lot.wallCoverings);
    }
    if (!haveTypes && !haveMap && records.empty()) {
        return lot;
    }
    if (!haveTypes || !haveMap) {
        throw std::runtime_error("lot: missing object type table or object map");
    }

    for (const auto& [id, candidates] : records) {
        const auto listed = lot.objectMap.typeIndexByObject.find(id);
        if (listed == lot.objectMap.typeIndexByObject.end()) {
            lot.staleRecords += candidates.size();
            continue;
        }
        const auto* type = lot.types.find(static_cast<std::uint16_t>(listed->second));
        if (!type) {
            lot.problems.push_back({id, "object map points at a missing type"});
            lot.staleRecords += candidates.size();
            continue;
        }
        const std::uint32_t wanted = type->kind == objt::kPersonKind ? types::kSavedPerson
                                     : (type->flag & 1)              ? types::kSavedObjectWithMaterial
                                                                     : types::kSavedObject;
        std::optional<LotObject> chosen;
        for (const auto* e : candidates) {
            if (e->key.type != wanted) {
                ++lot.staleRecords;
                continue;
            }
            std::optional<xobj::SavedObject> state;
            try {
                state = xobj::parse(package.read(*e), lot.objectMap.version, recordKind(e->key.type));
            } catch (const std::exception& ex) {
                lot.problems.push_back({id, ex.what()});
                continue;
            }
            if (static_cast<std::uint16_t>(state->data[kObjectIdField]) != id) {
                lot.problems.push_back({id, "object data field 0x0B is not the object's id"});
            }
            chosen = LotObject{id, e->key.type, *type, std::move(*state)};
        }
        if (!chosen) {
            lot.problems.push_back({id, "no record of the type the object table asks for"});
            continue;
        }
        lot.objects.push_back(std::move(*chosen));
    }
    for (const auto& [id, typeIndex] : lot.objectMap.typeIndexByObject) {
        if (!records.contains(id)) {
            lot.problems.push_back({id, "listed object has no saved record"});
        }
    }
    return lot;
}

int gridIndex(const Lot& lot, std::int64_t level) {
    // Clamped so that a broken save's values cannot overflow.
    constexpr std::int64_t kLimit = 1000;
    const std::int64_t lowest = lot.roomGraph ? lot.roomGraph->firstLevel : 0;
    return static_cast<int>(std::clamp<std::int64_t>(level - lowest, -kLimit, kLimit));
}

int gridLevel(const Lot& lot, std::int32_t storey) {
    return gridIndex(lot, std::int64_t{storey} - 1);
}

namespace {

void dropLevelsBelowGround(wallgraph::WallGraph& graph) {
    std::erase_if(graph.points, [](const wallgraph::Point& p) { return p.level < 0; });
    std::unordered_set<std::int32_t> kept;
    for (const auto& p : graph.points) {
        kept.insert(p.id);
    }
    std::erase_if(graph.walls, [&](const wallgraph::Wall& w) { return !kept.contains(w.from) || !kept.contains(w.to); });
    const std::int64_t levels = std::int64_t{graph.levels} + std::min(0, graph.firstLevel);
    graph.levels = static_cast<std::int32_t>(std::max<std::int64_t>(levels, 0));
    graph.firstLevel = 0;
}

} // namespace

Lot fromGroundStorey(const Lot& lot) {
    Lot view = lot;
    const int ground = groundGridLevel(lot);
    if (ground <= 0) {
        return view;
    }
    const std::uint32_t levels = lot.grids.at(kFloorGridInstance).levels;
    for (auto& [instance, grid] : view.grids) {
        if (grid.levels != levels) {
            continue;
        }
        const std::size_t below = std::min(grid.elements.size(), static_cast<std::size_t>(ground) * grid.width * grid.height * grid.elementSize);
        grid.elements.erase(grid.elements.begin(), grid.elements.begin() + static_cast<std::ptrdiff_t>(below));
        grid.levels -= static_cast<std::uint32_t>(ground);
    }
    for (auto& [instance, grid] : view.listGrids) {
        if (grid.levels != levels) {
            continue;
        }
        const std::size_t below = std::min(grid.cells.size(), static_cast<std::size_t>(ground) * grid.width * grid.height);
        grid.cells.erase(grid.cells.begin(), grid.cells.begin() + static_cast<std::ptrdiff_t>(below));
        grid.levels -= static_cast<std::uint32_t>(ground);
    }
    if (view.roomGraph) {
        dropLevelsBelowGround(*view.roomGraph);
    }
    for (auto& [instance, graph] : view.layerGraphs) {
        dropLevelsBelowGround(graph);
    }
    std::erase_if(view.fencePosts, [](const fencepostlayer::Post& p) { return p.level < 0; });
    return view;
}

int groundGridLevel(const Lot& lot) {
    const int level = gridIndex(lot, 0);
    const auto floors = lot.grids.find(kFloorGridInstance);
    return floors != lot.grids.end() && level >= 0 && static_cast<std::uint32_t>(level) < floors->second.levels ? level : 0;
}

} // namespace ts2::content
