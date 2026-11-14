// LotLife's part in Buy mode: the catalog, and making, holding, putting back and selling objects.
#include "lot_life.hpp"

#include "diag/log.hpp"
#include "sim/idle_spot.hpp"
#include "sim/snap.hpp"
#include "sim/vm.hpp"
#include "sim/vm_execution.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <memory>

namespace app {

namespace {

constexpr std::size_t kDirectionData = 1, kAllowedHeightsData = 4;
constexpr std::int16_t kOnTheFloor = 0x01;

bool isMultiTile(const ts2::sim::SimObject& o) { return o.definition.field(ts2::objd::field::kMultiTileMasterId) != 0; }

constexpr std::size_t kContainerData = 2, kSlotData = 3, kWallPlacementData = 0x0D;

// Whether the built walls (not fences) of the tile at (x, y) on `level` (1 = ground) are where the
// object's wall placement flags (data 0x0D) need them facing `facing` (see catalog::wallsWhereNeeded).
bool requiredWallsThere(const ts2::sim::World& world, const ts2::sim::SimObject& object, float x, float y, std::int32_t level,
                        std::int16_t facing) {
    const std::uint8_t sides = world.sightWalls().at(level - 1, static_cast<int>(x), static_cast<int>(y));
    return ts2::sim::catalog::wallsWhereNeeded(object.data[kWallPlacementData], sides, facing);
}

} // namespace

const std::vector<ts2::sim::catalog::Item>& LotLife::buyItems() {
    if (!buyItems_) {
        buyItems_ = ts2::sim::catalog::buyItems(sim_->code());
        ts2::diag::info(std::format("buy mode: {} objects in the catalog", buyItems_->size()));
    }
    return *buyItems_;
}

std::string LotLife::catalogText(const ts2::sim::catalog::Item& item, std::size_t line) {
    return sim_->code().catalogString(item.group, item.catalogStrings, line).value_or(std::string());
}

std::int16_t LotLife::makeForBuying(std::uint32_t guid) {
    try {
        // A multi-tile object is made as its parts, and is held by its lead.
        const ts2::sim::ObjectType* type = sim_->code().objectType(guid);
        const bool multiTile = type != nullptr && type->definition.isMultiTileMaster();
        ts2::sim::SimObject* made = multiTile ? sim_->vm().createMultiTile(guid) : sim_->vm().create(guid);
        if (made == nullptr) {
            return 0;
        }
        ts2::diag::step(std::format("buy mode: made {} {}", made->definition.name, made->id));
        return made->id;
    } catch (const std::exception& e) {
        ts2::diag::warning(std::format("buy mode: object {:08x} can't be made: {}", guid, e.what()));
        return 0;
    }
}

bool LotLife::standsOnFloor(std::int16_t object) {
    const auto* found = sim_->world().find(object);
    return found != nullptr && (found->data[kAllowedHeightsData] & kOnTheFloor) != 0;
}

LotLife::BuySpot LotLife::buySpot(const std::array<float, 3>& scenePoint, int storey) const {
    const auto [x, y] = routePoint(scenePoint);
    // The middle of the tile under the point.
    const auto middle = [](float v) { return static_cast<std::int32_t>((std::floor(v) + 0.5f) * 65536.0f); };
    return {middle(x), middle(y), storey};
}

std::int16_t LotLife::leadOf(std::int16_t object) {
    const auto* found = sim_->world().find(object);
    if (found == nullptr || !isMultiTile(*found) || found->data[ts2::sim::kLeadTileField] <= 0) {
        return object;
    }
    return found->data[ts2::sim::kLeadTileField];
}

std::vector<std::int16_t> LotLife::partsOf(std::int16_t object) {
    const std::int16_t lead = leadOf(object);
    const auto* found = sim_->world().find(lead);
    if (found == nullptr || !isMultiTile(*found)) {
        return {object};
    }
    return sim_->world().idsIncludingRemoved([&](const ts2::sim::SimObject& o) {
        return !o.removed && isMultiTile(o) && o.data[ts2::sim::kLeadTileField] == lead;
    });
}

// A multi-tile object, its lead at `target`, may stay where every part is on the lot with nothing in
// its way (Snap's placement test for each part), every part above the ground has a floor under all
// of it, and no wall runs between two parts side by side. The object must allow the floor (its
// lead's data 4 bit 0). Not done yet, as for objects of one tile: the game's other placement rules.
bool LotLife::multiTileFits(const ts2::sim::SimObject& lead, const ts2::sim::SnapTarget& target) {
    auto& world = sim_->world();
    if ((lead.data[kAllowedHeightsData] & kOnTheFloor) == 0 || ts2::sim::placementCode(world, lead, target)) {
        return false;
    }
    const auto parts = partsOf(lead.id);
    std::vector<const ts2::sim::SimObject*> placed;
    for (const std::int16_t id : parts) {
        const auto* part = world.find(id);
        if (part == nullptr || part->x == ts2::sim::kOffLot || part->y == ts2::sim::kOffLot) {
            return false;
        }
        placed.push_back(part);
    }
    const auto& pattern = world.floorPattern();
    for (const auto* part : placed) {
        if (part->level <= 1) {
            continue;
        }
        const int tileX = part->x >> 16, tileY = part->y >> 16;
        bool floor = static_cast<bool>(pattern);
        for (int q = 0; floor && q < 4; ++q) {
            const auto p = pattern(part->level - 1, tileY, tileX, q);  // the floor grid counts tiles the other way round
            floor = p && *p != 0;
        }
        if (!floor) {
            return false;
        }
    }
    for (const auto* a : placed) {
        for (const auto* b : placed) {
            const int dx = (b->x >> 16) - (a->x >> 16), dy = (b->y >> 16) - (a->y >> 16);
            if (a->id < b->id && a->level == b->level && std::abs(dx) + std::abs(dy) == 1 &&
                ts2::sim::wallBetween(world, a->level - 1, static_cast<float>(a->x) / 65536.0f, static_cast<float>(a->y) / 65536.0f,
                                      static_cast<float>(b->x) / 65536.0f, static_cast<float>(b->y) / 65536.0f)) {
                return false;
            }
        }
    }
    return true;
}

bool LotLife::holdAt(std::int16_t object, const BuySpot& spot, std::int16_t direction) {
    auto& world = sim_->world();
    ts2::sim::SimObject* found = world.find(leadOf(object));
    if (found == nullptr) {
        return false;
    }
    ts2::sim::SnapTarget target;
    target.x = spot.x;
    target.y = spot.y;
    target.level = spot.level;
    target.facing = direction;
    target.checkRoom = false;
    target.checkWalls = false;
    ts2::sim::takeOutOfSlot(world, *found);  // off the surface it was on
    ts2::sim::snapTo(world, *found, target);  // the other parts of a multi-tile object follow
    if (isMultiTile(*found)) {
        return multiTileFits(*found, target);
    }
    const float x = static_cast<float>(spot.x) / 65536.0f, y = static_cast<float>(spot.y) / 65536.0f;
    if (!requiredWallsThere(world, *found, x, y, spot.level, direction)) {
        return false;
    }
    const auto& fits = world.objectFits();
    return !fits || fits(*found, static_cast<float>(spot.x) / 65536.0f, static_cast<float>(spot.y) / 65536.0f, spot.level, direction);
}

std::optional<LotLife::Placed> LotLife::placed(std::int16_t object) {
    const auto* found = sim_->world().find(leadOf(object));
    if (found == nullptr || found->x == ts2::sim::kOffLot || found->y == ts2::sim::kOffLot) {
        return std::nullopt;
    }
    Placed out{{found->x, found->y, found->level}, found->data[kDirectionData], std::nullopt};
    if (found->data[kContainerData] > 0) {
        out.slot = SlotSpot{found->data[kContainerData], found->data[kSlotData]};
    }
    return out;
}

void LotLife::putBack(std::int16_t object, const Placed& where) {
    if (where.slot) {
        holdInSlot(object, *where.slot);
    } else {
        holdAt(object, where.spot, where.direction);
    }
}

bool LotLife::goesOnSurfaces(std::int16_t object) {
    const auto* found = sim_->world().find(object);
    return found != nullptr && (found->data[kAllowedHeightsData] & ~kOnTheFloor) != 0;
}

std::optional<LotLife::SlotSpot> LotLife::freeSlot(std::int16_t held, std::int16_t container) {
    auto& world = sim_->world();
    const auto* object = world.find(held);
    const auto* surface = world.find(container);
    if (object == nullptr || surface == nullptr || surface->isPerson() || surface->removed) {
        return std::nullopt;
    }
    const auto parts = partsOf(held);
    if (std::find(parts.begin(), parts.end(), container) != parts.end() || surface->data[kContainerData] == held) {
        return std::nullopt;
    }
    const ts2::sim::ObjectType* type = sim_->code().objectType(surface->definition.guid());
    if (type == nullptr) {
        return std::nullopt;
    }
    const auto heights = static_cast<std::uint16_t>(object->data[kAllowedHeightsData]);
    for (std::size_t i = 0; i < type->containerSlotEntries.size() && i < surface->slots.size(); ++i) {
        if (!ts2::sim::catalog::fitsSlotHeight(heights, type->containerSlotEntries[i].containerKind)) {
            continue;
        }
        const std::int16_t inside = surface->slots[i];
        if (inside == 0 || inside == held || world.find(inside) == nullptr) {
            return SlotSpot{container, static_cast<std::int16_t>(i)};
        }
    }
    return std::nullopt;
}

void LotLife::holdInSlot(std::int16_t object, const SlotSpot& where) {
    constexpr std::size_t kRoomData = 0x1D;
    auto& world = sim_->world();
    ts2::sim::SimObject* held = world.find(object);
    ts2::sim::SimObject* surface = world.find(where.container);
    if (held == nullptr || surface == nullptr || where.slot < 0 || static_cast<std::size_t>(where.slot) >= surface->slots.size()) {
        return;
    }
    // As Snap Into a slot puts it: out of its old slot and off the tiles, at the surface's place,
    // facing the surface's way.
    ts2::sim::takeOutOfSlot(world, *held);
    surface->slots[static_cast<std::size_t>(where.slot)] = held->id;
    held->data[kContainerData] = surface->id;
    held->data[kSlotData] = where.slot;
    held->x = surface->x;
    held->y = surface->y;
    held->level = surface->level;
    held->tile.reset();
    held->data[kDirectionData] = surface->data[kDirectionData];
    held->heading.reset();
    held->data[kRoomData] = surface->data[kRoomData];
    world.markFootprintChanged(held->id);
}

bool LotLife::movable(std::int16_t object) {
    const auto* found = sim_->world().find(object);
    if (found == nullptr || found->removed || found->isPerson() || found->x == ts2::sim::kOffLot || found->y == ts2::sim::kOffLot) {
        return false;
    }
    // In a slot: on a surface it may be picked up, in a Sim's hand not.
    if (found->data[kContainerData] > 0) {
        const auto* container = sim_->world().find(found->data[kContainerData]);
        if (container == nullptr || container->isPerson()) {
            return false;
        }
    }
    const std::uint32_t guid = found->definition.guid();
    if (std::find(std::begin(ts2::sim::vm_detail::kProtectedGuids), std::end(ts2::sim::vm_detail::kProtectedGuids), guid) != std::end(ts2::sim::vm_detail::kProtectedGuids)) {
        return false;
    }
    // A part of a multi-tile object is moved with the rest when its master is sold in Buy mode.
    if (isMultiTile(*found)) {
        const auto* master = buyDefinition(object);
        return master != nullptr && ts2::sim::catalog::isBuyItem(*master);
    }
    return ts2::sim::catalog::isBuyItem(found->definition);
}

const ts2::objd::ObjectDefinition* LotLife::buyDefinition(std::int16_t object) {
    const auto* found = sim_->world().find(object);
    if (found == nullptr) {
        return nullptr;
    }
    auto& code = sim_->code();
    const ts2::sim::ObjectType* type = code.objectType(found->definition.guid());
    if (type == nullptr) {
        return nullptr;
    }
    if (isMultiTile(*found)) {
        const ts2::sim::ObjectType* master = code.multiTileMaster(*type);
        return master != nullptr ? &master->definition : nullptr;
    }
    return &type->definition;
}

std::int32_t LotLife::currentValue(std::int16_t object) {
    const auto* found = sim_->world().find(leadOf(object));
    return found != nullptr ? std::max<std::int32_t>(0, found->data[ts2::sim::catalog::kCurrentValueData]) : 0;
}

void LotLife::setCurrentValue(std::int16_t object, std::int32_t value) {
    if (auto* found = sim_->world().find(leadOf(object))) {
        found->data[ts2::sim::catalog::kCurrentValueData] = static_cast<std::int16_t>(std::clamp<std::int32_t>(value, 0, 32767));
    }
}

std::uint32_t LotLife::typeOf(std::int16_t object) {
    const auto* found = sim_->world().find(object);
    return found != nullptr ? found->definition.guid() : 0;
}

void LotLife::removeObject(std::int16_t object) {
    constexpr std::size_t kRoomData = 0x1D;
    auto& world = sim_->world();
    // Every part of a multi-tile object goes with it.
    for (const std::int16_t id : partsOf(object)) {
        ts2::sim::SimObject* found = world.findIncludingRemoved(id);
        if (found == nullptr) {
            continue;
        }
        ts2::sim::takeOutOfSlot(world, *found);
        // Off the lot, as Create Object's "out of world" leaves an object: no tile and room -1.
        found->x = found->y = ts2::sim::kOffLot;
        found->tile.reset();
        found->data[kRoomData] = -1;
        world.markFootprintChanged(found->id);
        world.remove(*found);
        try {
            sim_->vm().tellSimsObjectGoes(id);
        } catch (const std::exception& e) {
            ts2::diag::warning(std::format("buy mode: the Sims couldn't forget object {}: {}", id, e.what()));
        }
    }
}

void LotLife::addFunds(std::int32_t amount) {
    auto globals = sim_->globals();
    ts2::sim::setFunds(globals, static_cast<std::int64_t>(ts2::sim::funds(globals)) + amount);
}

// Objects on the lot (not Sims, not taken off it or held in a slot) on a storey, with their tile in
// wall graph coordinates (route x is the graph's y).
namespace {

struct OnTile {
    const ts2::sim::SimObject* object;
    int x, y;  // the tile, in wall graph coordinates
};

std::vector<OnTile> objectsOn(ts2::sim::World& world, std::int32_t storey) {
    std::vector<OnTile> out;
    world.nextAfter(0, [&](const ts2::sim::SimObject& o) {
        if (!o.removed && !o.isPerson() && o.x != ts2::sim::kOffLot && o.y != ts2::sim::kOffLot && o.level == storey &&
            o.data[kContainerData] <= 0) {
            const int routeX = static_cast<int>(std::floor(static_cast<float>(o.x) / 65536.0f));
            const int routeY = static_cast<int>(std::floor(static_cast<float>(o.y) / 65536.0f));
            out.push_back({&o, routeY, routeX});
        }
        return false;
    });
    return out;
}

} // namespace

bool LotLife::objectAcross(std::int32_t storey, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1) {
    // The two tiles the edge parts, in wall graph coordinates.
    const bool alongY = x0 == x1;
    const std::array<int, 2> a = alongY ? std::array<int, 2>{x0 - 1, std::min(y0, y1)} : std::array<int, 2>{std::min(x0, x1), y0 - 1};
    const std::array<int, 2> b = alongY ? std::array<int, 2>{x0, std::min(y0, y1)} : std::array<int, 2>{std::min(x0, x1), y0};
    std::map<std::int16_t, std::array<bool, 2>> sides;  // by lead: has a part on a, on b
    for (const auto& on : objectsOn(sim_->world(), storey)) {
        const bool onA = on.x == a[0] && on.y == a[1], onB = on.x == b[0] && on.y == b[1];
        if (onA || onB) {
            auto& s = sides[leadOf(on.object->id)];
            s[0] = s[0] || onA;
            s[1] = s[1] || onB;
        }
    }
    return std::any_of(sides.begin(), sides.end(), [](const auto& s) { return s.second[0] && s.second[1]; });
}

std::int16_t LotLife::objectNeedingWalls(const ts2::content::Lot& candidate) {
    const auto after = ts2::sim::WallTiles::builtWalls(candidate);
    const auto& world = sim_->world();
    std::int16_t needing = 0;
    for (std::int32_t storey = 1; storey <= 8 && needing == 0; ++storey) {
        for (const auto& on : objectsOn(sim_->world(), storey)) {
            const auto& o = *on.object;
            const std::int16_t flags = o.data[kWallPlacementData];
            if ((flags & 0xF) == 0) {
                continue;
            }
            const float x = static_cast<float>(o.x) / 65536.0f, y = static_cast<float>(o.y) / 65536.0f;
            const std::int16_t facing = o.data[kDirectionData];
            // Only an object that has its walls now can lose them.
            if (requiredWallsThere(world, o, x, y, o.level, facing) &&
                !ts2::sim::catalog::wallsWhereNeeded(flags, after.at(o.level - 1, static_cast<int>(x), static_cast<int>(y)), facing)) {
                needing = o.id;
                break;
            }
        }
    }
    return needing;
}

void LotLife::setFloors(const ts2::array3d::Array3D& floors, int ground) {
    // As the simulation was given the saved floors (see LotSimulation): a copy of its own.
    sim_->world().setFloorPattern([grid = std::make_shared<const ts2::array3d::Array3D>(floors), ground](int level, int x, int y,
                                                                                                         int quarter) -> std::optional<std::uint16_t> {
        constexpr int kQuarters = 4;
        const auto index = static_cast<std::int64_t>(level) + ground;
        if (quarter < 0 || quarter >= kQuarters || index < 0 || index > std::numeric_limits<std::int32_t>::max() ||
            !grid->contains(static_cast<std::int32_t>(index), x, y)) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(grid->i16(static_cast<std::int32_t>(index), x, y, static_cast<std::size_t>(quarter)));
    });
}

} // namespace app
