#include "sim/route_world.hpp"

#include "diag/log.hpp"
#include "sim/object_footprint.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
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

// A door part's route items: where the leaf swings on its side of the wall (a cost to cross, with a
// padded copy) and, from the part in front of the wall, the doorway square through it. They belong to
// the door's lead part.
std::vector<RouteItem> doorRouteItems(const SimObject& object, const SimObject& lead, const RouteModel* model) {
    std::vector<RouteItem> items;
    int tileX = 0, tileY = 0;
    if (&lead != &object &&
        !footprintTileOf(tiles(lead.x), tiles(lead.y), lead.data[1], tiles(object.x), tiles(object.y), tileX, tileY)) {
        return items;
    }
    const float x = tiles(object.x), y = tiles(object.y);
    const auto owner = static_cast<std::uint32_t>(lead.id);
    // The door's sides: the front part's tile and the tile behind the wall, from this part's place in it.
    const auto place = [&](float along) {
        const float ly = along - static_cast<float>(tileY);
        switch ((object.data[1] & 7) / 2) {
        case 0: return std::pair{x, y + ly};
        case 1: return std::pair{x - ly, y};
        case 2: return std::pair{x, y - ly};
        default: return std::pair{x + ly, y};
        }
    };
    const auto [ax, ay] = place(0);
    const auto [bx, by] = place(1);
    const RouteDoor door{ax, ay, bx, by};
    if (model != nullptr && model->footprint) {
        for (const auto& r : placeFootprint(footprintTile(*model->footprint, lead.data[9], tileX, tileY), x, y, object.data[1])) {
            RouteItem swing;
            swing.rect = r;
            swing.cost = kSwingCost;
            swing.type = kSwingType;
            swing.kind = RouteItemKind::Area;
            swing.owner = owner;
            swing.door = door;
            items.push_back(swing);
            swing.rect = {r.x0 - kPadding, r.y0 - kPadding, r.x1 + kPadding, r.y1 + kPadding};
            swing.cost = kSwingPaddingCost;
            items.push_back(swing);
        }
    }
    if (tileY != 0) {
        return items;
    }
    for (const auto& r : placeFootprint({doorwayRect()}, x, y, object.data[1])) {
        RouteItem doorway;
        doorway.rect = r;
        doorway.cost = kDoorwayCost;
        doorway.kind = RouteItemKind::Solid;
        doorway.owner = owner;
        doorway.door = door;
        items.push_back(doorway);
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
    const ModelBone bone{place[3], place[7], std::atan2(place[4], place[0])};
    if (!std::isfinite(bone.x) || !std::isfinite(bone.y) || !std::isfinite(bone.yaw)) {
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
                                        const RouteModel* model) {
    std::vector<RouteItem> items;
    if (leftOut(object, type)) {
        return items;
    }
    if (isDoor(type)) {
        return doorRouteItems(object, lead, model);
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
    // Routing covers the ground storey and up. The level below the ground is not routed yet.
    // A broken save's level count is held to the levels the floor grid really has.
    const int lowest = std::clamp(lot.roomGraph->firstLevel, -1, 0);
    const std::int64_t graphLevels = std::int64_t{lot.roomGraph->levels} + lowest;
    const std::int64_t gridLevels = std::int64_t{floors->second.levels} + lowest;
    const int levels = static_cast<int>(std::clamp<std::int64_t>(std::min(graphLevels, gridLevels), 0, kMaxRouteLevels));
    if (std::min(graphLevels, gridLevels) > kMaxRouteLevels) {
        diag::warnOnce("routing: levels past 64 are not routed");
    }
    const auto gridOf = [&](std::uint32_t instance) {
        const auto it = lot.grids.find(instance);
        return it != lot.grids.end() && it->second.elementSize == 4 ? &it->second : nullptr;
    };
    const array3d::Array3D* heights = gridOf(content::kFloorHeightGridInstance);
    const array3d::Array3D* surface = gridOf(content::kSurfaceGridInstance);
    for (int level = 0; level < levels; ++level) {
        RouteGrid& grid = grids_.emplace_back(static_cast<float>(width), static_cast<float>(height));
        for (const auto& item : wallRouteItems(*lot.roomGraph, lot.wallCoverings, level, fenceBlocks, width, height)) {
            grid.add(item);
        }
        for (const auto& item : floorRouteItems(floors->second, heights, surface, lot.floorPatterns, level, lowest, width, height)) {
            grid.add(item);
        }
        for (const auto& item : postRouteItems(lot.fencePosts, level, postFootprint)) {
            grid.add(item);
        }
    }
}

namespace {

// An object's route items now and the level (0 = ground) they go on, level -1 for none.
struct PlacedItems {
    int level = -1;
    std::vector<RouteItem> items;
};

PlacedItems currentItems(World& world, CodeSource& code, const std::map<std::uint32_t, std::string>& objectModels,
                         const RouteModelLookup& models, const SimObject& object, int levels) {
    const ObjectType* type = code.objectType(object.definition.guid());
    const int level = object.level - 1;
    if (type == nullptr || level < 0 || level >= levels) {
        return {};
    }
    // A multi-tile part routes as its tile of the lead's model.
    const SimObject* lead = object.data[0x13] > 0 ? world.find(object.data[0x13]) : nullptr;
    if (lead == nullptr) {
        lead = &object;
    }
    const auto modelName = objectModels.find(static_cast<std::uint32_t>(lead->id));
    const RouteModel* model = modelName != objectModels.end() && models ? models(modelName->second) : nullptr;
    return {level, objectRouteItems(object, *lead, *type, code, model)};
}

} // namespace

void RouteWorld::addObjects(World& world, CodeSource& code, const content::Lot& lot, const RouteModelLookup& models) {
    objectModels_ = lot.objectModels;
    world.nextAfter(0, [&](const SimObject& object) {
        const PlacedItems placed = currentItems(world, code, objectModels_, models, object, levels());
        for (const auto& item : placed.items) {
            grids_[static_cast<std::size_t>(placed.level)].add(item);
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
        PlacedItems now = object != nullptr ? currentItems(world, code, objectModels_, models, *object, levels()) : PlacedItems{};
        std::erase_if(now.items, [](const RouteItem& item) { return item.kind != RouteItemKind::Object || !RouteGrid::accepts(item); });
        const auto owner = static_cast<std::uint32_t>(id);
        for (std::size_t level = 0; level < grids_.size(); ++level) {
            const auto& wanted = static_cast<int>(level) == now.level ? now.items : none;
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
        changed.push_back(static_cast<int>(level));
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
        if (old->second.level >= 0 && old->second.level < levels()) {
            grids_[static_cast<std::size_t>(old->second.level)].remove(RouteItemKind::Soft, owner);
            changed.push_back(old->second.level);
        }
        rings_.erase(old);
    }
    if (rings && rings->level >= 0 && rings->level < levels()) {
        for (const RouteItem& item : simRingItems(rings->square, rings->target, owner[0])) {
            grids_[static_cast<std::size_t>(rings->level)].add(item);
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
    return level >= 0 && level < levels() ? &grids_[static_cast<std::size_t>(level)] : nullptr;
}

} // namespace ts2::sim
