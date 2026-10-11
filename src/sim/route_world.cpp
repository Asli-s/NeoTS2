#include "sim/route_world.hpp"

#include "sim/object_footprint.hpp"

#include <cmath>

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
    if (model != nullptr && model->footprint) {
        for (const auto& r : placeFootprint(footprintTile(*model->footprint, lead.data[9], tileX, tileY), x, y, object.data[1])) {
            RouteItem swing;
            swing.rect = r;
            swing.cost = kSwingCost;
            swing.type = kSwingType;
            swing.kind = RouteItemKind::Area;
            swing.owner = owner;
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
        items.push_back(doorway);
    }
    return items;
}

} // namespace

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
        return items;
    }
    const auto rects = placeFootprint(footprintRects(shape), tiles(object.x), tiles(object.y), d[1]);
    for (const auto& r : rects) {
        RouteItem core;
        core.rect = r;
        core.cost = routing.cost;
        core.type = routing.type;
        core.kind = RouteItemKind::Object;
        core.owner = static_cast<std::uint32_t>(object.id);
        items.push_back(core);
        if (!routing.passable) {
            RouteItem padded = core;
            padded.rect = {r.x0 - kPadding, r.y0 - kPadding, r.x1 + kPadding, r.y1 + kPadding};
            padded.type = routing.type - 1;
            items.push_back(padded);
        }
    }
    return items;
}

RouteWorld::RouteWorld(const content::Lot& lot, const FenceBlocks& fenceBlocks, const PostFootprint& postFootprint) {
    const auto floors = lot.grids.find(content::kFloorGridInstance);
    if (!lot.roomGraph || floors == lot.grids.end()) {
        return;
    }
    // The floor grid runs along the wall graph's axes; route x is graph y.
    const int width = static_cast<int>(floors->second.height), height = static_cast<int>(floors->second.width);
    for (int level = 0; level < lot.roomGraph->levels; ++level) {
        RouteGrid& grid = grids_.emplace_back(static_cast<float>(width), static_cast<float>(height));
        for (const auto& item : wallRouteItems(*lot.roomGraph, lot.wallCoverings, level, fenceBlocks)) {
            grid.add(item);
        }
        for (const auto& item : floorRouteItems(floors->second, lot.floorPatterns, level, width, height)) {
            grid.add(item);
        }
        for (const auto& item : postRouteItems(lot.fencePosts, level, postFootprint)) {
            grid.add(item);
        }
    }
}

void RouteWorld::addObjects(World& world, CodeSource& code, const content::Lot& lot, const RouteModelLookup& models) {
    world.nextAfter(0, [&](const SimObject& object) {
        const ObjectType* type = code.objectType(object.definition.guid());
        if (type == nullptr) {
            return false;
        }
        // A multi-tile part routes as its tile of the lead's model.
        const SimObject* lead = object.data[0x13] > 0 ? world.find(object.data[0x13]) : nullptr;
        if (lead == nullptr) {
            lead = &object;
        }
        const auto modelName = lot.objectModels.find(static_cast<std::uint32_t>(lead->id));
        const RouteModel* model = modelName != lot.objectModels.end() && models ? models(modelName->second) : nullptr;
        const int level = object.level - 1;
        if (level < 0 || level >= levels()) {
            return false;
        }
        for (const auto& item : objectRouteItems(object, *lead, *type, code, model)) {
            grids_[static_cast<std::size_t>(level)].add(item);
        }
        return false;
    });
}

const RouteGrid* RouteWorld::level(int level) const {
    return level >= 0 && level < levels() ? &grids_[static_cast<std::size_t>(level)] : nullptr;
}

} // namespace ts2::sim
