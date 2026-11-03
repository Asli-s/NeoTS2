#include "sim/route_world.hpp"

#include "diag/log.hpp"
#include "sim/object_footprint.hpp"
#include "sim/route_portals.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace ts2::sim {

namespace {

constexpr int kDoorType = 8, kWindowType = 9, kStairType = 12;  // OBJD field 9
constexpr std::uint16_t kAllowedInRouteMarker = 0x93;          // OBJD 0x4B for those that do route
constexpr std::uint16_t kAllowIntersection = 5;                // OBJf entry
constexpr float kPadding = 0.125f;                              // the padded copy's margin
// A door's swing area (Sims may cross it, at a cost) and the doorway through its wall.
constexpr float kSwingCost = 17, kSwingPaddingCost = 13;
constexpr int kSwingType = 1;
constexpr float kDoorwayCost = 1;

float tiles(std::int32_t fixed) {
    return static_cast<float>(fixed) / 65536.0f;
}

// Whether the game leaves an object out of routing whatever its flags.
bool leftOut(const SimObject& object, const ObjectType& type) {
    if (object.isPerson() || object.x < 0 || object.y < 0) {
        return true;
    }
    if (object.data[2] != 0) {
        return true;  // held in another object's slot
    }
    const int objectType = type.definition.field(objd::field::kType);
    if ((objectType == kWindowType || objectType == kStairType) && type.definition.field(0x4B) != kAllowedInRouteMarker) {
        return true;
    }
    return (static_cast<std::uint16_t>(object.data[0x2A]) & 0x1000) != 0;
}

bool isDoor(const ObjectType& type) {
    return type.definition.field(objd::field::kType) == kDoorType && type.definition.field(0x4B) != kAllowedInRouteMarker;
}

// Whether a way through a door crosses a diagonal wall: its heading in eighths of a turn, rounded
// down from half an eighth past it, is odd.
bool diagonalWay(const RouteDoor& way) {
    constexpr float kPi = 3.14159265f;
    const float eighths = std::atan2(way.by - way.ay, way.bx - way.ax) / (2 * kPi) * 8 + 0.5f;
    return (static_cast<int>(std::floor(eighths)) & 1) != 0;
}

// A door's route items, all made by its lead and filed under it. Where the leaf swings is each tile
// its model's masks name, a cost to cross with a padded copy, belonging to the way whose end is
// nearest. Each way through the door gets a doorway square halfway between its ends, and on a
// diagonal wall two more a quarter tile either side along the way. A door with no way through adds
// nothing.
std::vector<RouteItem> doorRouteItems(const SimObject& object, const SimObject& lead, const RouteModel* model,
                                      std::span<const RouteDoor> ways) {
    constexpr float kDiagonalStep = 0.25f;
    std::vector<RouteItem> items;
    if (&lead != &object || ways.empty()) {
        return items;
    }
    const auto owner = static_cast<std::uint32_t>(lead.id);
    const auto nearestWay = [&](float x, float y) -> const RouteDoor& {
        const RouteDoor* best = &ways.front();
        float bestDistance = std::numeric_limits<float>::max();
        for (const RouteDoor& way : ways) {
            for (const auto [ex, ey] : {std::pair{way.ax, way.ay}, std::pair{way.bx, way.by}}) {
                if (const float d = std::hypot(ex - x, ey - y); d < bestDistance) {
                    bestDistance = d;
                    best = &way;
                }
            }
        }
        return *best;
    };
    if (model != nullptr && model->footprint) {
        for (const auto [tileX, tileY] : footprintMaskTiles(*model->footprint, lead.data[9])) {
            auto local = footprintTile(*model->footprint, lead.data[9], tileX, tileY);
            for (RouteRect& r : local) {
                r = {r.x0 + static_cast<float>(tileX), r.y0 + static_cast<float>(tileY), r.x1 + static_cast<float>(tileX),
                     r.y1 + static_cast<float>(tileY)};
            }
            for (const auto& r : placeFootprint(local, tiles(lead.x), tiles(lead.y), lead.data[1])) {
                RouteItem swing;
                swing.rect = r;
                swing.cost = kSwingCost;
                swing.type = kSwingType;
                swing.kind = RouteItemKind::Area;
                swing.owner = owner;
                swing.door = nearestWay((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2);
                items.push_back(swing);
                swing.rect = {r.x0 - kPadding, r.y0 - kPadding, r.x1 + kPadding, r.y1 + kPadding};
                swing.cost = kSwingPaddingCost;
                items.push_back(swing);
            }
        }
    }
    for (const RouteDoor& way : ways) {
        const float x = (way.ax + way.bx) / 2, y = (way.ay + way.by) / 2;
        const auto add = [&](float atX, float atY) {
            RouteItem doorway;
            doorway.rect = doorwaySquare(atX, atY);
            doorway.cost = kDoorwayCost;
            doorway.kind = RouteItemKind::Solid;
            doorway.owner = owner;
            doorway.door = way;
            items.push_back(doorway);
        };
        add(x, y);
        if (diagonalWay(way)) {
            const float length = std::hypot(way.bx - way.ax, way.by - way.ay);
            const float dx = (way.bx - way.ax) / length * kDiagonalStep, dy = (way.by - way.ay) / length * kDiagonalStep;
            add(x + dx, y + dy);
            add(x - dx, y - dy);
        }
    }
    return items;
}

// A place as a 3 x 4 matrix, rows first.
using Place = std::array<float, 12>;

Place placeOf(const std::array<float, 3>& t, const std::array<float, 4>& q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),     t[0],
            2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),     t[1],
            2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y), t[2]};
}

Place times(const Place& a, const Place& b) {
    Place r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            float v = j == 3 ? a[static_cast<std::size_t>(i * 4 + 3)] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[static_cast<std::size_t>(i * 4 + k)] * b[static_cast<std::size_t>(k * 4 + j)];
            }
            r[static_cast<std::size_t>(i * 4 + j)] = v;
        }
    }
    return r;
}

bool sameName(const std::string& lower, const std::string& other) {
    return lower.size() == other.size() && std::equal(lower.begin(), lower.end(), other.begin(), [](char a, char b) {
               return a == static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
           });
}

} // namespace

std::optional<ModelBone> posedModelBone(const RouteModel& model, const std::string& name, std::span<const cobject::SavedJoint> saved) {
    if (saved.empty()) {
        const auto found = model.bones.find(name);
        return found != model.bones.end() ? std::optional<ModelBone>(found->second) : std::nullopt;
    }
    const auto node = std::find_if(model.nodes.begin(), model.nodes.end(), [&](const RouteModel::Node& n) { return !n.name.empty() && n.name == name; });
    if (node == model.nodes.end()) {
        return std::nullopt;
    }
    // The node and its parents, root first. Parents come before their children, so this ends.
    std::vector<std::size_t> chain;
    for (auto i = static_cast<int>(node - model.nodes.begin()); i >= 0; i = model.nodes[static_cast<std::size_t>(i)].parent) {
        chain.push_back(static_cast<std::size_t>(i));
        if (model.nodes[static_cast<std::size_t>(i)].parent >= i) {
            break;
        }
    }
    Place place = placeOf({0, 0, 0}, {0, 0, 0, 1});
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const auto& n = model.nodes[*it];
        std::array<float, 3> t = n.translation;
        std::array<float, 4> q = n.rotation;
        if (!n.name.empty()) {
            for (const auto& j : saved) {
                if (sameName(n.name, j.name) && std::ranges::all_of(j.translation, [](float v) { return std::isfinite(v); }) &&
                    std::ranges::all_of(j.rotation, [](float v) { return std::isfinite(v); })) {
                    t = j.translation;
                    q = j.rotation;
                    break;
                }
            }
        }
        place = times(place, placeOf(t, q));
    }
    // A broken model's own nodes can still make the place non-finite.
    const ModelBone bone{place[3], place[7], std::atan2(place[4], place[0]), place[11]};
    if (!std::isfinite(bone.x) || !std::isfinite(bone.y) || !std::isfinite(bone.yaw) || !std::isfinite(bone.z)) {
        return std::nullopt;
    }
    return bone;
}

std::vector<RouteRect> objectFootprintAt(const SimObject& object, const SimObject& lead, const ObjectType& type,
                                         const RouteModel* model, float x, float y, std::int16_t direction) {
    const auto& d = object.data;
    FootprintShape shape;
    shape.square = (d[8] & 0x80) != 0;
    const std::uint16_t size = type.definition.field(0x43);
    shape.squareSize = size != 0 ? static_cast<float>(size) / 16.0f : 1.0f;
    shape.insets = type.definition.field(0x4D);
    if (model != nullptr) {
        shape.masks = model->footprint ? &*model->footprint : nullptr;
        shape.modelBox = model->extent;
    }
    shape.graphic = lead.data[9];  // a part asks its lead, which uses its own graphic
    if (&lead != &object &&
        !footprintTileOf(tiles(lead.x), tiles(lead.y), lead.data[1], tiles(object.x), tiles(object.y), shape.tileX, shape.tileY)) {
        return {};
    }
    return placeFootprint(footprintRects(shape), x, y, direction);
}

std::vector<RouteItem> objectRouteItems(const SimObject& object, const SimObject& lead, const ObjectType& type, CodeSource& code,
                                        const RouteModel* model, std::span<const RouteDoor> doorWays) {
    std::vector<RouteItem> items;
    if (leftOut(object, type)) {
        return items;
    }
    if (isDoor(type)) {
        return doorRouteItems(object, lead, model, doorWays);
    }
    const auto* functions = code.objectFunctions(type);
    const bool allowsIntersection =
        functions != nullptr && kAllowIntersection < functions->size() && (*functions)[kAllowIntersection].action != 0;
    const auto& d = object.data;
    const FootprintRouting routing =
        footprintRouting(static_cast<std::uint16_t>(d[8]), d[10], static_cast<std::uint16_t>(d[0x28]),
                         type.definition.field(objd::field::kType), allowsIntersection);
    if (!routing.added) {
        return items;
    }

    for (const auto& r : objectFootprintAt(object, lead, type, model, tiles(object.x), tiles(object.y), d[1])) {
        RouteItem core;
        core.rect = r;
        core.cost = routing.cost;
        core.type = routing.type;
        core.kind = RouteItemKind::Object;
        core.owner = static_cast<std::uint32_t>(object.id);
        core.dynamic = routing.dynamic && !routing.passable;
        core.exactFootprint = (static_cast<std::uint16_t>(d[0x28]) & 0x4000) != 0;
        items.push_back(core);
        if (!routing.passable) {
            RouteItem padded = core;
            padded.rect = {r.x0 - kPadding, r.y0 - kPadding, r.x1 + kPadding, r.y1 + kPadding};
            padded.type = routing.type - 1;
            padded.padding = true;
            items.push_back(padded);
        }
    }
    return items;
}

constexpr std::int64_t kMaxRouteLevels = 64;

RouteWorld::RouteWorld(const content::Lot& lot, const FenceBlocks& fenceBlocks, const PostFootprint& postFootprint) {
    const auto floors = lot.grids.find(content::kFloorGridInstance);
    if (!lot.roomGraph || floors == lot.grids.end()) {
        return;
    }
    // The floor grid runs along the wall graph's axes. Route x is graph y.
    const int width = static_cast<int>(floors->second.height), height = static_cast<int>(floors->second.width);
    // Routing covers every level from the lowest, the level below the ground holding the pools.
    // A broken save's level count is held to the levels the floor grid really has.
    const int lowest = std::clamp(lot.roomGraph->firstLevel, -1, 0);
    const std::int64_t graphLevels = std::int64_t{lot.roomGraph->levels} + lowest;
    const std::int64_t gridLevels = std::int64_t{floors->second.levels} + lowest;
    const int levels = static_cast<int>(std::clamp<std::int64_t>(std::min(graphLevels, gridLevels), lowest, kMaxRouteLevels));
    if (std::min(graphLevels, gridLevels) > kMaxRouteLevels) {
        diag::warnOnce("routing: levels from 64 up are not routed");
    }
    const auto gridOf = [&](std::uint32_t instance) {
        const auto it = lot.grids.find(instance);
        return it != lot.grids.end() && it->second.elementSize == 4 ? &it->second : nullptr;
    };
    const array3d::Array3D* heights = gridOf(content::kFloorHeightGridInstance);
    const array3d::Array3D* surface = gridOf(content::kSurfaceGridInstance);
    const auto waterGrid = lot.flatGrids.find(content::kWaterHeightGridInstance);
    const terrain::Array2D* water = waterGrid != lot.flatGrids.end() ? &waterGrid->second : nullptr;
    lowest_ = lowest;
    for (int level = lowest; level < levels; ++level) {
        RouteGrid& grid = grids_.emplace_back(static_cast<float>(width), static_cast<float>(height));
        for (const auto& item : wallRouteItems(*lot.roomGraph, lot.wallCoverings, level, fenceBlocks, width, height)) {
            grid.add(item);
        }
        for (const auto& item : floorRouteItems(floors->second, heights, surface, lot.floorPatterns, level, lowest, width, height, water)) {
            grid.add(item);
        }
        for (const auto& item : postRouteItems(lot.fencePosts, level, postFootprint)) {
            grid.add(item);
        }
    }
}

namespace {

// An object's route items now and the level (0 = ground, -1 below it) they go on.
struct PlacedItems {
    int level = 0;
    std::vector<RouteItem> items;
};

PlacedItems currentItems(World& world, CodeSource& code, const std::map<std::uint32_t, std::string>& objectModels,
                         const RouteModelLookup& models, const SimObject& object, int lowest, int levels) {
    const ObjectType* type = code.objectType(object.definition.guid());
    const int level = object.level - 1;
    if (type == nullptr || level < lowest || level >= lowest + levels) {
        return {};
    }
    // A multi-tile part routes as its tile of the lead's model.
    const SimObject* lead = object.data[0x13] > 0 ? world.find(object.data[0x13]) : nullptr;
    if (lead == nullptr) {
        lead = &object;
    }
    const auto modelName = objectModels.find(static_cast<std::uint32_t>(lead->id));
    const RouteModel* model = modelName != objectModels.end() && models ? models(modelName->second) : nullptr;
    std::vector<RouteDoor> ways;
    if (isDoor(*type) && lead == &object) {
        for (const StairPortal& p : doorPortals(world, code, object)) {
            const RouteDoor way{p.from.x, p.from.y, p.to.x, p.to.y};
            const RouteDoor back{way.bx, way.by, way.ax, way.ay};
            const bool finite = std::isfinite(way.ax) && std::isfinite(way.ay) && std::isfinite(way.bx) && std::isfinite(way.by);
            if (finite && std::find(ways.begin(), ways.end(), way) == ways.end() && std::find(ways.begin(), ways.end(), back) == ways.end()) {
                ways.push_back(way);
            }
        }
    }
    return {level, objectRouteItems(object, *lead, *type, code, model, ways)};
}

} // namespace

void RouteWorld::addObjects(World& world, CodeSource& code, const std::map<std::uint32_t, std::string>& objectModels, const RouteModelLookup& models) {
    objectModels_ = objectModels;
    world.nextAfter(0, [&](const SimObject& object) {
        const PlacedItems placed = currentItems(world, code, objectModels_, models, object, lowest_, levels());
        for (const auto& item : placed.items) {
            grid(placed.level)->add(item);
        }
        return false;
    });
}

std::vector<int> RouteWorld::updateObjects(World& world, CodeSource& code, const RouteModelLookup& models, const std::set<std::int16_t>& objects) {
    // Each level is changed once, for all objects whose items there differ from what they had.
    std::vector<std::vector<std::uint32_t>> stale(grids_.size());
    std::vector<std::vector<RouteItem>> added(grids_.size());
    const std::vector<RouteItem> none;
    for (const std::int16_t id : objects) {
        const SimObject* object = world.find(id);
        PlacedItems now = object != nullptr ? currentItems(world, code, objectModels_, models, *object, lowest_, levels()) : PlacedItems{};
        std::erase_if(now.items, [](const RouteItem& item) { return item.kind != RouteItemKind::Object || !RouteGrid::accepts(item); });
        const auto owner = static_cast<std::uint32_t>(id);
        for (std::size_t level = 0; level < grids_.size(); ++level) {
            const auto& wanted = static_cast<int>(level) + lowest_ == now.level ? now.items : none;
            const auto had = grids_[level].itemsOf(RouteItemKind::Object, owner);
            if (had.size() != wanted.size() || !std::is_permutation(had.begin(), had.end(), wanted.begin())) {
                stale[level].push_back(owner);
                added[level].insert(added[level].end(), wanted.begin(), wanted.end());
            }
        }
    }
    std::vector<int> changed;
    for (std::size_t level = 0; level < grids_.size(); ++level) {
        if (stale[level].empty()) {
            continue;
        }
        std::sort(stale[level].begin(), stale[level].end());
        grids_[level].remove(RouteItemKind::Object, stale[level]);
        for (const auto& item : added[level]) {
            grids_[level].add(item);
        }
        changed.push_back(static_cast<int>(level) + lowest_);
    }
    return changed;
}

std::vector<int> RouteWorld::ringSim(std::int16_t sim, const std::optional<SimRings>& rings) {
    const auto old = rings_.find(sim);
    if (old == rings_.end() ? !rings : (rings && old->second == *rings)) {
        return {};
    }
    std::vector<int> changed;
    const std::uint32_t owner[] = {static_cast<std::uint32_t>(sim)};
    if (old != rings_.end()) {
        if (RouteGrid* g = grid(old->second.level)) {
            g->remove(RouteItemKind::Soft, owner);
            changed.push_back(old->second.level);
        }
        rings_.erase(old);
    }
    if (RouteGrid* g = rings ? grid(rings->level) : nullptr) {
        for (const RouteItem& item : simRingItems(rings->square, rings->target, owner[0])) {
            g->add(item);
        }
        rings_.emplace(sim, *rings);
        if (std::ranges::find(changed, rings->level) == changed.end()) {
            changed.push_back(rings->level);
        }
    }
    return changed;
}

std::vector<std::int16_t> RouteWorld::ringedSims() const {
    std::vector<std::int16_t> ids;
    for (const auto& [id, rings] : rings_) {
        ids.push_back(id);
    }
    return ids;
}

const RouteGrid* RouteWorld::level(int level) const {
    const int at = level - lowest_;
    return at >= 0 && at < levels() ? &grids_[static_cast<std::size_t>(at)] : nullptr;
}

RouteGrid* RouteWorld::grid(int level) {
    const int at = level - lowest_;
    return at >= 0 && at < levels() ? &grids_[static_cast<std::size_t>(at)] : nullptr;
}

} // namespace ts2::sim
