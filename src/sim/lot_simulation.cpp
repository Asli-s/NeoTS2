#include "sim/lot_simulation.hpp"
#include "sim/forget_object.hpp"

#include "content/build_scripts.hpp"
#include "content/install.hpp"
#include "content/lot.hpp"
#include "content/neighborhood.hpp"
#include "content/scripts.hpp"
#include "content/wants.hpp"
#include "diag/log.hpp"
#include "diag/timing.hpp"
#include "formats/resource_types.hpp"
#include "sim/lua_runtime.hpp"
#include "sim/object_reset.hpp"
#include "sim/overlay_events.hpp"
#include "sim/pie_menu.hpp"
#include "sim/route_destinations.hpp"
#include "sim/route_goals.hpp"
#include "sim/route_portals.hpp"
#include "sim/snap.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <format>
#include <optional>
#include <set>
#include <vector>

namespace ts2::sim {

namespace {

const auto programStart = std::chrono::steady_clock::now();

// The seed the game makes from the clock: the time in seconds since 1970 xor (ms | ms << 16), where ms
// counts the milliseconds since the program started.
std::uint32_t clockSeed() {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto ms = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - programStart).count());
    return static_cast<std::uint32_t>(seconds) ^ (ms | (ms << 16));
}

// Scripts may run this many primitives in one tick before they are stopped.
constexpr std::size_t kMaxStepsPerRun = 10000;
constexpr std::size_t kMaxLoggedStops = 32;
constexpr std::size_t kMaxLoggedStarts = 5000;  // a long play session's worth. The log file has its own cap too

// The install's object packages and the lot's neighborhood's character files.
content::ResourceIndex loadCode(const std::filesystem::path& install, const std::filesystem::path& lot) {
    content::ResourceIndex index;
    for (const auto& path : content::objectLoadOrder(content::findProducts(install))) {
        index.addPackage(path);
    }
    content::addCharacters(index, content::neighborhoodOfLot(lot));
    return index;
}

std::shared_ptr<LuaRuntime> loadScripts(const std::filesystem::path& install, CodeSource& code) {
    const auto package = content::objectScriptsPackage(install);
    if (!package) {
        return nullptr;
    }
    const auto loaded = content::loadObjectScripts(*package);
    auto runtime = std::make_shared<LuaRuntime>();
    runtime->installedPacks = content::installedPacksMask(content::findProducts(install));
    runtime->load(loaded.modules, loaded.scripts, &code);
    return runtime;
}

} // namespace

LotSimulation::LotSimulation(const std::filesystem::path& install, const std::filesystem::path& lot,
                             std::function<void(const StepInfo&)> onLoadStep)
    : index_(loadCode(install, lot)), code_(index_), vm_(code_, {kMaxStepsPerRun, 64}, &world_),
      installedPacks_(content::installedPacksMask(content::findProducts(install))), motiveUpdate_(code_, installedPacks_) {
    vm_.onInteractionStart = [this](const SimObject& sim, const QueuedInteraction& started) { logInteractionStart(sim, started); };
    vm_.resetSim = [this](SimObject& sim) {
        if (const ObjectType* type = code_.objectType(sim.definition.guid())) {
            resetAfterFailure(sim, *type);
        }
    };
    lot_ = std::make_unique<const content::Lot>(content::loadLot(lot));
    const content::Lot& saved = *lot_;
    objectModels_ = saved.objectModels;
    savedJoints_ = saved.savedJoints;
    report_ = buildWorld(saved, code_, world_);
    world_.setWalls(WallTiles::fromLot(saved));
    world_.setSightWalls(WallTiles::builtWalls(saved));
    const content::BuildScripts buildScripts = content::loadBuildScripts(content::findProducts(install));
    std::vector<World::WallSection> sections;
    std::unordered_map<std::int32_t, const wallgraph::Wall*> walls;
    std::optional<wallgraph::PointIndex> points;
    if (saved.roomGraph) {
        for (const auto& w : saved.roomGraph->walls) {
            walls.emplace(w.id, &w);
        }
        points.emplace(*saved.roomGraph);
    }
    for (const auto& e : saved.wallCoverings) {
        if (!buildScripts.isRoomWall(e.type)) {
            continue;
        }
        World::WallSection section;
        if (const auto wall = walls.find(static_cast<std::int32_t>(e.wall)); wall != walls.end() && points) {
            const auto* from = (*points)(wall->second->from);
            const auto* to = (*points)(wall->second->to);
            if (from != nullptr && to != nullptr) {
                section = {*from, *to, true};
            }
        }
        sections.push_back(section);
    }
    world_.setWallSections(std::move(sections));
    if (const auto graph = saved.layerGraphs.find(content::kLayerGraphInstances[1]); graph != saved.layerGraphs.end()) {
        world_.setWeatherRegions(RoomMap(graph->second));
    }
    world_.setOutside(OutsideGrid(saved, world_.weatherRegions()));
    if (const auto floors = saved.grids.find(content::kFloorGridInstance); floors != saved.grids.end()) {
        const int ground = content::groundGridLevel(saved);
        // A copy of its own: the loaded lot is freed once the routing world is built.
        world_.setFloorPattern([grid = std::make_shared<const array3d::Array3D>(floors->second), ground](int level, int x, int y, int quarter) -> std::optional<std::uint16_t> {
            constexpr int kQuarters = 4;
            const auto index = static_cast<std::int64_t>(level) + ground;
            if (quarter < 0 || quarter >= kQuarters || index < 0 || index > std::numeric_limits<std::int32_t>::max() ||
                !grid->contains(static_cast<std::int32_t>(index), x, y)) {
                return std::nullopt;
            }
            return static_cast<std::uint16_t>(grid->i16(static_cast<std::int32_t>(index), x, y, static_cast<std::size_t>(quarter)));
        });
    }
    world_.setPlatformRegion(platformRegions(saved));
    world_.setNeighborRemap(saved.neighborRemap ? std::optional(saved.neighborRemap->neighborIds) : std::nullopt,
                            saved.unreadNeighborRemap);
    std::copy_n(saved.globals.begin(), std::min(saved.globals.size(), vm_.globals.size()), vm_.globals.begin());
    // A lot saved without a random state starts from the clock, as the game seeds its generator.
    vm_.rng.seed(saved.randomState ? *saved.randomState : clockSeed());
    updateTimeOfDay(vm_.globals);
    const auto hood = content::neighborhoodPackage(content::neighborhoodOfLot(lot));
    if (std::filesystem::exists(hood)) {
        const content::Neighborhood loaded = content::loadNeighborhood(hood);
        findHousehold(loaded, lot);
        world_.setNeighborhood(savedNeighborhood(loaded, code_, content::neighborhoodIds(content::neighborhoodOfLot(lot).parent_path())));
        // Global 0x28, the current neighborhood's id, is what scripts ask NeighborhoodManager about.
        constexpr std::size_t kCurrentNeighborhoodGlobal = 0x28;
        if (loaded.identity && kCurrentNeighborhoodGlobal < vm_.globals.size()) {
            vm_.globals[kCurrentNeighborhoodGlobal] = static_cast<std::int16_t>(loaded.identity->id);
        }
        if (const auto number = content::lotNumber(lot)) {
            world_.setLotNumber(*number);
        }
        if (const auto number = content::lotNumber(lot); number && loaded.lotTypes.contains(*number)) {
            world_.setLotType(loaded.lotTypes.at(*number));
        }
        if (const auto number = content::lotNumber(lot); number && loaded.lotShapes.contains(*number)) {
            const auto& shape = loaded.lotShapes.at(*number);
            world_.setLotShape({static_cast<float>(shape.width), static_cast<float>(shape.depth), shape.beachSide});
        }
        world_.weather() = loaded.weather.value_or(weather::Weather{});
        findCharacterFiles();
    }
    if (const auto wants = content::wantsPackage(install)) {
        auto defs = std::make_shared<content::WantDefinitions>(content::loadWantDefinitions(*wants));
        if (const auto tuning = content::wantTuningPackage(install)) {
            const std::string text = content::loadWantTuning(*tuning);
            defs->categories = content::parseWantCategories(text);
            if (const auto influence = content::parseInfluenceCurve(text)) {
                defs->influenceLevels = influence->inverse();
            }
        }
        world_.setWantDefinitions(std::move(defs));
    }
    world_.setScripts(loadScripts(install, code_));
    vm_.onStep = std::move(onLoadStep);
    startObjects();
    vm_.onStep = nullptr;
}

void LotSimulation::setRouteSources(RouteSources sources) {
    routeSources_ = std::move(sources);
    world_.setModelBones([this](std::int16_t object, const std::string& bone) -> std::optional<ModelBone> {
        const auto name = objectModels_.find(static_cast<std::uint32_t>(object));
        const RouteModel* model = name != objectModels_.end() && routeSources_->models ? routeSources_->models(name->second) : nullptr;
        if (model == nullptr) {
            return std::nullopt;
        }
        // As the object was posed when the lot was saved (a dining chair pushed in keeps its seat slid).
        const auto saved = savedJoints_.find(static_cast<std::uint32_t>(object));
        return posedModelBone(*model, bone, saved != savedJoints_.end() ? std::span<const cobject::SavedJoint>(saved->second)
                                                                             : std::span<const cobject::SavedJoint>());
    });
    world_.setSlotRouter([this](const SimObject& sim, const SimObject& owner, const slot::Entry& slot,
                                const std::vector<std::optional<ModelBone>>& joints) { return routeToSlot(sim, owner, slot, joints); });
    world_.setPlacementSpots([this](const SimObject& placed, const SimObject& owner, const slot::Entry& slot,
                                    std::span<const std::optional<ModelBone>> joints) {
        return placementSpots(placed, owner, slot, joints);
    });
    world_.setObjectFits([this](const SimObject& placed, float x, float y, std::int32_t level, std::int16_t direction) {
        return objectFits(placed, x, y, level, direction);
    });
    world_.setStepBlocker([this](const SimObject& sim, float x, float y, std::int32_t level) { return stepBlocker(sim, x, y, level); });
}

const RouteWorld* LotSimulation::routeWorld() {
    if (!routeSources_) {
        return nullptr;
    }
    if (!routeWorld_) {
        if (!lot_) {
            return nullptr;
        }
        RouteWorld built(*lot_, routeSources_->fenceBlocks, routeSources_->postFootprint);
        built.addObjects(world_, code_, *lot_, routeSources_->models);
        routeWorld_ = std::move(built);
        world_.takeFootprintChanges();  // already as they are now
        lot_.reset();
        routeLinks_.clear();
    } else if (auto changed = world_.takeFootprintChanges(); !changed.empty()) {
        for (const int level : routeWorld_->updateObjects(world_, code_, routeSources_->models, changed)) {
            routeLinks_.erase(level);
        }
    }
    return routeWorld_->levels() > 0 ? &*routeWorld_ : nullptr;
}

// The game plans these as it does a Sim's goals, the placed object in the Sim's place and without
// the wall tests. Its object is not in its own way.
std::vector<std::pair<float, float>> LotSimulation::placementSpots(const SimObject& placed, const SimObject& owner, const slot::Entry& slot,
                                                                   std::span<const std::optional<ModelBone>> joints) {
    std::vector<std::pair<float, float>> spots;
    const RouteWorld* routes = routeWorld();
    const RouteGrid* grid = routes != nullptr ? routes->level(owner.level - 1) : nullptr;
    if (grid == nullptr) {
        return spots;
    }
    const SimObject* lead = placed.data[0x13] != 0 ? world_.find(placed.data[0x13]) : nullptr;
    const auto placedLead = static_cast<std::uint32_t>(lead != nullptr ? lead->id : placed.id);
    SlotGoalWorld goalWorld;
    goalWorld.walls = &world_.walls();
    goalWorld.placing = true;
    goalWorld.floorCost = [grid](int, float x, float y) { return floorCostAt(*grid, x, y); };
    goalWorld.canStand = [grid, placedLead](int, float x, float y, std::int16_t, GoalFit) { return canStandAt(*grid, x, y, placedLead); };
    RouteRandom random(++routePlans_ * 2654435761u);
    std::vector<RouteGoal> goals = isBoneSlot(slot)    ? boneSlotGoals(owner, slot, joints, placed, goalWorld, random)
                                   : isPointSlot(slot) ? pointSlotGoal(owner, slot, goalWorld)
                                                       : sweptSlotGoals(owner, slot, placed, goalWorld, random);
    std::stable_sort(goals.begin(), goals.end(), [](const RouteGoal& a, const RouteGoal& b) { return a.score > b.score; });
    for (const RouteGoal& g : goals) {
        spots.emplace_back(g.x, g.y);
    }
    return spots;
}

// The game's object placement test, for an object of one tile. The spot must be on the lot and on a
// routed level, and the tile must have floor: the ground always has, a tile above it needs a floor
// pattern on every quarter or an object with data 0x62 bit 0 on it (unless placement flags data 0x2A
// has 0x200 or data 0x52 has 4). Room placement data 0x1E 1 needs the spot outdoors, 2 indoors. No
// object or Sim may be in the way (see objectInTheWay), and allowed heights data 4 must allow the
// floor (bit 0).
// Not run yet: the game's second floor flag, the pool below, terrain, pool, slope, floor-or-ground
// (data 0x2A bits 0 and 1), lot edge, roof, ceiling and wall tests, and its rules for objects on the
// same tile and near doors. Multi-tile objects always pass.
bool LotSimulation::objectFits(const SimObject& placed, float x, float y, std::int32_t level, std::int16_t direction) {
    constexpr std::size_t kHeightsField = 4, kRoomPlacementField = 0x1E, kPlacementFlagsField = 0x2A, kFlags3Field = 0x52;
    constexpr std::size_t kFloorStandInField = 0x62, kMultiTileIdDefinitionField = 0x0A;
    constexpr std::int16_t kOnTheFloor = 0x01, kNeedsNoFloor = 0x200, kNeedsNoFloor3 = 0x04;
    const ObjectType* type = code_.objectType(placed.definition.guid());
    if (type == nullptr || type->definition.field(kMultiTileIdDefinitionField) != 0) {
        return true;
    }
    const RouteWorld* routes = routeWorld();
    if (routes == nullptr) {
        return true;
    }
    const RouteGrid* grid = routes->level(level - 1);
    if (grid == nullptr || !(x >= 0 && y >= 0 && x < grid->width() && y < grid->height())) {
        return false;
    }
    const int tileX = static_cast<int>(x), tileY = static_cast<int>(y);
    const auto& d = placed.data;
    if (level > 1 && !(d[kPlacementFlagsField] & kNeedsNoFloor) && !(d[kFlags3Field] & kNeedsNoFloor3)) {
        // The floor grid counts tiles the other way round.
        const auto& pattern = world_.floorPattern();
        bool floor = static_cast<bool>(pattern);
        for (int q = 0; floor && q < 4; ++q) {
            const auto p = pattern(level - 1, tileY, tileX, q);
            floor = p && *p != 0;
        }
        if (!floor) {
            floor = world_.nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& o) {
                return !o.removed && o.id != placed.id && o.tile && o.tile->level == level && o.tile->x == tileX &&
                       o.tile->y == tileY && (o.data[kFloorStandInField] & 1) != 0;
            }) != nullptr;
        }
        if (!floor) {
            return false;
        }
    }
    if (const std::int16_t placement = d[kRoomPlacementField]; placement == 1 || placement == 2) {
        const RoomInfo* room = world_.rooms().room(world_.rooms().roomAtPosition(level - 1, x, y));
        const std::int32_t region = room != nullptr ? room->layerRegion[placement == 1 ? 0 : 1] : -1;
        if (region >= 0 && (placement == 1) != (region == 0)) {
            return false;
        }
    }
    if (objectInTheWay(placed, *type, x, y, level, direction)) {
        return false;
    }
    return (d[kHeightsField] & kOnTheFloor) != 0;
}

// Whether something stands in the way of `placed` at (x, y) facing `direction`, as the game's
// placement test finds it. Objects on the tiles round the spot count when their footprints overlap
// the placed one's, Sims when they stand within 0.3 tiles of it. Left out: objects in a slot or a
// hand, doors and windows, pairs that either one's "allow intersection" function (OBJf entry 5) lets
// through, pairs whose data 0x2A bit 0x1000 differs, other objects when either one has object data 8
// bit 0x02 or 0x04, and Sims when the placed object has data 8 bit 0x04 or 0x10. Sims with data 8
// bit 0x01 or data 0x28 bit 0x1000 are left out too (the game also asks another Sim flag there, not
// known yet), and every Sim when the placed object has data 0x28 bit 0x4000 (the game then tests the
// Sim's own footprint).
bool LotSimulation::objectInTheWay(const SimObject& placed, const ObjectType& type, float x, float y, std::int32_t level,
                                   std::int16_t direction) {
    constexpr std::size_t kContainerField = 2, kFlagsField = 8, kFlags2Field = 0x28, kPlacementFlagsField = 0x2A, kLeadField = 0x13;
    constexpr std::int16_t kSimGroupFlag = 0x1000, kIgnoresSims = 0x14, kIgnoresObjects = 0x06, kSkippedSim = 0x01;
    constexpr std::uint16_t kSimsByFootprint = 0x4000;
    constexpr float kSimClearance = 0.3f;
    constexpr int kDoorType = 8, kWindowType = 9;
    const auto model = [&](const SimObject& o) -> const RouteModel* {
        const auto name = objectModels_.find(static_cast<std::uint32_t>(o.id));
        return name != objectModels_.end() && routeSources_ && routeSources_->models ? routeSources_->models(name->second) : nullptr;
    };
    const std::vector<RouteRect> mine = objectFootprintAt(placed, placed, type, model(placed), x, y, direction);
    if (mine.empty()) {
        return false;
    }
    // The tiles round the spot's tile, or round every footprint rectangle for objects with OBJD 0x4E set.
    float x0 = std::floor(x), y0 = std::floor(y), x1 = x0, y1 = y0;
    if (type.definition.field(0x4E) != 0) {
        for (const RouteRect& r : mine) {
            x0 = std::min(x0, std::floor(r.x0));
            y0 = std::min(y0, std::floor(r.y0));
            x1 = std::max(x1, std::floor(r.x1));
            y1 = std::max(y1, std::floor(r.y1));
        }
    }
    const auto near = [&](const SimObject& o) {
        return o.tile && o.tile->level == level && o.tile->x >= x0 - 1 && o.tile->x <= x1 + 1 && o.tile->y >= y0 - 1 &&
               o.tile->y <= y1 + 1;
    };
    const auto passes = [&](const SimObject& o) {
        return ((placed.data[kPlacementFlagsField] ^ o.data[kPlacementFlagsField]) & kSimGroupFlag) != 0 ||
               intersectionsFor(placed)(static_cast<std::uint32_t>(o.id)).has_value() ||
               intersectionsFor(o)(static_cast<std::uint32_t>(placed.id)).has_value();
    };
    return world_.nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& o) {
        if (o.removed || o.id == placed.id || o.x == kOffLot || o.data[kContainerField] > 0 || !near(o)) {
            return false;
        }
        if (o.isPerson()) {
            if ((placed.data[kFlagsField] & kIgnoresSims) || (o.data[kFlagsField] & kSkippedSim) ||
                (o.data[kFlags2Field] & kSimGroupFlag) || (static_cast<std::uint16_t>(placed.data[kFlags2Field]) & kSimsByFootprint) ||
                passes(o)) {
                return false;
            }
            const float px = static_cast<float>(o.x) / 65536.0f, py = static_cast<float>(o.y) / 65536.0f;
            return std::ranges::any_of(mine, [&](const RouteRect& r) {
                if (px < r.x0 - kSimClearance || px > r.x1 + kSimClearance || py < r.y0 - kSimClearance || py > r.y1 + kSimClearance) {
                    return false;
                }
                if ((px >= r.x0 && px <= r.x1) || (py >= r.y0 && py <= r.y1)) {
                    return true;
                }
                const float dx = px < r.x0 ? r.x0 - px : px - r.x1, dy = py < r.y0 ? r.y0 - py : py - r.y1;
                return dx * dx + dy * dy < kSimClearance * kSimClearance;
            });
        }
        const ObjectType* other = code_.objectType(o.definition.guid());
        if (other == nullptr || ((placed.data[kFlagsField] | o.data[kFlagsField]) & kIgnoresObjects) != 0) {
            return false;
        }
        const int otherType = other->definition.field(objd::field::kType);
        if (otherType == kDoorType || otherType == kWindowType || passes(o)) {
            return false;
        }
        const SimObject* lead = o.data[kLeadField] > 0 ? world_.find(o.data[kLeadField]) : nullptr;
        if (lead == nullptr) {
            lead = &o;
        }
        const std::vector<RouteRect> theirs = objectFootprintAt(o, *lead, *other, model(*lead), static_cast<float>(o.x) / 65536.0f,
                                                                 static_cast<float>(o.y) / 65536.0f, o.data[1]);
        return std::ranges::any_of(theirs, [&](const RouteRect& t) {
            return std::ranges::any_of(mine, [&](const RouteRect& m) { return m.overlaps(t); });
        });
    }) != nullptr;
}

// What blocks a walking Sim's step to (x, y) on `level`, as the game places a walking Sim each tick.
// The step is tested. When it fails, the same test runs where the Sim stands now, and when that
// fails too the step is taken anyway, so a Sim already in a bad spot can always walk out of it.
// Only a step that fails from a spot that passes is blocked, by the first Sim or object found
// (see placeTest). 0 when the step is taken.
std::int16_t LotSimulation::stepBlocker(const SimObject& sim, float x, float y, std::int32_t level) {
    std::optional<RouteIntersects> made;  // made only when a Sim or a dynamic item is met
    const RouteIntersects intersects = [&](std::uint32_t owner) {
        if (!made) {
            made = intersectionsFor(sim);
        }
        return (*made)(owner);
    };
    const auto test = [&](float atX, float atY) { return placeTest(sim, atX, atY, level, intersects); };
    return blockedStep(test, x, y, sim.x / 65536.0f, sim.y / 65536.0f);
}

// The game's test of a walking Sim's place at (x, y) on `level`: the first thing in the way, none
// when the place passes. Another Sim within 0.283 tiles counts, standing or walking, by the rules
// goals use for Sims in the way (see routeToSlot). Sims held in a slot or a hand don't. Then an
// object whose footprint overlaps the Sim's square (see objectInTheWayOfSim). The floor and wall
// parts of the game's test are left out: walks follow paths planned on these grids.
std::optional<std::int16_t> LotSimulation::placeTest(const SimObject& sim, float x, float y, std::int32_t level,
                                                     const RouteIntersects& intersects) {
    constexpr float kSimRadius = 0.283f;
    constexpr std::size_t kContainerField = 2;
    constexpr std::int16_t kPassFlags = 0x14, kSimGroupFlag = 0x1000;
    const SimObject* other = world_.nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& o) {
        if (!o.isPerson() || o.removed || o.id == sim.id || o.level != level || o.x == kOffLot || o.data[kContainerField] > 0) {
            return false;
        }
        if (std::hypot(o.x / 65536.0f - x, o.y / 65536.0f - y) >= kSimRadius) {
            return false;
        }
        if ((o.data[8] & kPassFlags) || (sim.data[8] & kPassFlags) || ((o.data[0x2A] ^ sim.data[0x2A]) & kSimGroupFlag)) {
            return false;
        }
        return !intersects(static_cast<std::uint32_t>(o.id)) && !intersectionsFor(o)(static_cast<std::uint32_t>(sim.id));
    });
    if (other != nullptr) {
        return other->id;
    }
    const RouteWorld* routes = routeWorld();
    const RouteGrid* grid = routes != nullptr ? routes->level(level - 1) : nullptr;
    if (grid == nullptr) {
        return std::nullopt;
    }
    const auto object = objectInTheWayOfSim(*grid, x, y, static_cast<std::uint32_t>(sim.id), intersects, GoalFit::Square);
    if (!object || *object == 0 || *object > static_cast<std::uint32_t>(std::numeric_limits<std::int16_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::int16_t>(*object);
}

// Whether `sim` may cross a stair, as the route planner asks once per plan: the check tree of the
// Portal function (OBJf entry 15) of the stair's first part (else its other one) runs as the Sim, with
// that part as stack object. False refuses the way. Crossing costs 1 + that part's data 0x35 / 100.
// The Sim's temps are kept, as the router keeps them around a route.
std::pair<bool, float> LotSimulation::stairCheck(const SimObject& sim, const StairPortal& stair) {
    constexpr std::size_t kPortalFunction = 15, kCostField = 0x35;
    for (const std::int16_t id : {stair.from.part, stair.to.part}) {
        SimObject* part = world_.find(id);
        const ObjectType* type = part != nullptr ? code_.objectType(part->definition.guid()) : nullptr;
        const auto* functions = type != nullptr ? code_.objectFunctions(*type) : nullptr;
        if (functions == nullptr || kPortalFunction >= functions->size() || (*functions)[kPortalFunction].action == 0) {
            continue;
        }
        const float cost = 1.0f + 0.01f * static_cast<float>(part->data[kCostField]);
        SimObject* me = world_.find(sim.id);
        const std::uint16_t check = (*functions)[kPortalFunction].check;
        if (me == nullptr || check == 0) {
            return {false, cost};
        }
        const auto temps = me->thread.temps;
        const RunResult r = vm_.runEvent(*me, type->group, check, part->id, {});
        me->thread.temps = temps;
        return {r.outcome != Outcome::True, cost};
    }
    return {false, 1.0f};
}

// Which objects with changing footprints let `sim` walk through them, as the game's route planner
// asks. Each object's "allow intersection" function (OBJf entry 5) runs at once, as that object, with
// the Sim as both its stack object and param 0 (some trees read one, some the other). True lets the
// Sim through, at a cost of 1 + the object's data 0x0A / 100. Each object is asked once per route.
RouteIntersects LotSimulation::intersectionsFor(const SimObject& sim) {
    constexpr std::size_t kAllowIntersection = 5;
    auto asked = std::make_shared<std::map<std::uint32_t, std::optional<float>>>();
    return [this, asked, simId = sim.id](std::uint32_t owner) -> std::optional<float> {
        if (const auto known = asked->find(owner); known != asked->end()) {
            return known->second;
        }
        std::optional<float> through;
        SimObject* object = world_.find(static_cast<std::int16_t>(owner));
        const ObjectType* type = object != nullptr ? code_.objectType(object->definition.guid()) : nullptr;
        const auto* functions = type != nullptr ? code_.objectFunctions(*type) : nullptr;
        if (functions != nullptr && kAllowIntersection < functions->size() && (*functions)[kAllowIntersection].action != 0) {
            const std::int16_t args[] = {simId};
            const RunResult r = vm_.runEvent(*object, type->group, (*functions)[kAllowIntersection].action, simId, args);
            if (r.outcome == Outcome::True) {
                through = 1.0f + 0.01f * static_cast<float>(object->data[0x0A]);
            }
        }
        (*asked)[owner] = through;
        return through;
    };
}

// Go To Routing Slot's route: the slot's goals (see route_goals.hpp) and the path search to the best
// one it reaches, on the owner's level.
SlotRoute LotSimulation::routeToSlot(const SimObject& sim, const SimObject& owner, const slot::Entry& slot,
                                     const std::vector<std::optional<ModelBone>>& joints) {
    constexpr std::int16_t kPathNotFound = 2, kNoValidGoals = 6, kDestinationOccupied = 7;
    constexpr int kDoorType = 8;
    SlotRoute route;
    const RouteWorld* routes = routeWorld();
    const int level = owner.level - 1;
    const RouteGrid* grid = routes != nullptr ? routes->level(level) : nullptr;
    if (grid == nullptr) {
        route.failure = kNoValidGoals;
        return route;
    }
    // A slot on another level is routed to over stairs whatever its flag 0x8000 says. The game sends a
    // Sim upstairs to bed with that flag clear. What the flag changes is not known yet.
    const RouteIntersects intersects = intersectionsFor(sim);
    SlotGoalWorld goalWorld;
    goalWorld.walls = &world_.walls();
    goalWorld.floorCost = [grid](int, float x, float y) { return floorCostAt(*grid, x, y); };
    // The game keeps only the object itself from blocking its goals. A multi-tile object's footprint
    // is filed under its lead, so a part's goals are blocked by the whole object.
    goalWorld.canStand = [grid, &intersects](int, float x, float y, std::int16_t id, GoalFit fit) {
        return canStandAt(*grid, x, y, static_cast<std::uint32_t>(id), intersects, fit);
    };
    goalWorld.hasFloor = [grid](int, float x, float y) { return floorAt(*grid, x, y); };
    goalWorld.doorOnTile = [this](int onLevel, int x, int y) {
        bool door = false;
        world_.nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& o) {
            const ObjectType* t = o.isPerson() ? nullptr : code_.objectType(o.definition.guid());
            door = t != nullptr && t->definition.field(objd::field::kType) == kDoorType && o.level - 1 == onLevel && o.x != kOffLot &&
                   static_cast<int>(std::floor(o.x / 65536.0)) == x && static_cast<int>(std::floor(o.y / 65536.0)) == y;
            return door;
        });
        return door;
    };
    // Another Sim where it stands now. Sims don't count against each other when either has object
    // data 8 flag 0x04 or 0x10 set, when their data 0x2A flag 0x1000 differs, or when either one's
    // "allow intersection" function lets the other through.
    goalWorld.simNear = [this, &sim, &intersects](int onLevel, float x, float y, std::int16_t ownerSim) {
        bool near = false;
        world_.nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& o) {
            constexpr float kSimRadius = 0.283f;
            constexpr std::int16_t kPassFlags = 0x14, kSimGroupFlag = 0x1000;
            if (!o.isPerson() || o.removed || o.id == sim.id || o.id == ownerSim || o.level - 1 != onLevel || o.x == kOffLot) {
                return false;
            }
            if (std::hypot(o.x / 65536.0f - x, o.y / 65536.0f - y) >= kSimRadius) {
                return false;
            }
            if ((o.data[8] & kPassFlags) || (sim.data[8] & kPassFlags) || ((o.data[0x2A] ^ sim.data[0x2A]) & kSimGroupFlag)) {
                return false;
            }
            near = !intersects(static_cast<std::uint32_t>(o.id)) && !intersectionsFor(o)(static_cast<std::uint32_t>(sim.id));
            return near;
        });
        return near;
    };
    // The objects on a tile that may be seats, with their Sit and Stand trees and their routing slots.
    goalWorld.seatsOnTile = [this](int onLevel, int x, int y) {
        std::vector<Seat> seats;
        world_.nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& o) {
            constexpr std::size_t kSitEntry = 0x1A, kStandEntry = 0x1B;
            constexpr std::int16_t kPassFlags = 0x14;
            if (o.removed || o.level - 1 != onLevel || o.x == kOffLot || (o.data[8] & kPassFlags) ||
                static_cast<int>(std::floor(o.x / 65536.0)) != x || static_cast<int>(std::floor(o.y / 65536.0)) != y) {
                return false;
            }
            const ObjectType* type = code_.objectType(o.definition.guid());
            const auto* functions = type != nullptr ? code_.objectFunctions(*type) : nullptr;
            const auto has = [&](std::size_t entry) { return functions != nullptr && entry < functions->size() && (*functions)[entry].action != 0; };
            Seat seat{&o, has(kSitEntry), has(kStandEntry), {}};
            if (seat.hasSit && type != nullptr) {
                for (const slot::Entry& entry : type->routingSlots) {
                    seat.slots.push_back({entry, slotJoints(world_, code_, o, entry, type->group)});
                }
            }
            seats.push_back(std::move(seat));
            return false;
        });
        return seats;
    };
    RouteRandom random(++routePlans_ * 2654435761u);
    bool seatTaken = false;
    GoalMisses misses;
    const SimObject base = slotGoalBase(world_, code_, owner);
    const bool swept = !isBoneSlot(slot) && !isPointSlot(slot);
    const std::vector<RouteGoal> goals = isBoneSlot(slot)    ? boneSlotGoals(base, slot, joints, sim, goalWorld, random, &misses)
                                         : isPointSlot(slot) ? pointSlotGoal(base, slot, goalWorld, &misses)
                                                             : sweptSlotGoals(base, slot, sim, goalWorld, random, &seatTaken);
    // With no goal the route fails: a sweep says only whether a seat was taken, the other slot kinds
    // why their goals were left out.
    if (goals.empty()) {
        route.failure = swept ? (seatTaken ? kDestinationOccupied : kNoValidGoals) : noGoalReason(misses);
        return route;
    }
    // The search aims at the goals' centre, as the game's mostly does.
    float aimX = 0, aimY = 0;
    for (const RouteGoal& g : goals) {
        aimX += g.x / static_cast<float>(goals.size());
        aimY += g.y / static_cast<float>(goals.size());
    }
    auto links = routeLinks_.find(level);
    if (links == routeLinks_.end()) {
        links = routeLinks_.emplace(level, RouteLinks::build(*grid)).first;
    }
    // Each door as the game reads it when a plan starts: its lead's data 0x35 and 0x16.
    std::vector<RouteDoorState> doors;
    std::set<std::uint32_t> doorOwners;
    const bool acrossLevels = sim.level != owner.level;
    std::vector<const RouteItem*> doorItems;
    for (int l = 0; l < routes->levels(); ++l) {
        if (const RouteGrid* g = routes->level(l); g != nullptr && (acrossLevels || l == level)) {
            for (const RouteItem& item : g->items()) {
                doorItems.push_back(&item);
            }
        }
    }
    for (const RouteItem* itemOf : doorItems) {
        const RouteItem& item = *itemOf;
        if (item.door && doorOwners.insert(item.owner).second) {
            if (const SimObject* o = world_.find(static_cast<std::int16_t>(item.owner))) {
                RouteDoorState d;
                d.owner = item.owner;
                d.extra = 1.0f + 0.01f * o->data[0x35];
                d.active[0] = (o->data[0x16] & 1) != 0;
                d.active[1] = (o->data[0x16] & 2) != 0;
                doors.push_back(d);
            }
        }
    }
    const RouteStart start[] = {{static_cast<float>(sim.x) / 65536.0f, static_cast<float>(sim.y) / 65536.0f, sim.level - 1}};
    std::optional<RoutePlan> plan;
    std::vector<StairPortal> stairs;
    if (!acrossLevels) {
        plan = planRoute(*grid, links->second, start, goals, aimX, aimY, doors, routeJitter_, random, intersects);
    } else {
        // Every level, joined by the lot's stairs, each as this Sim may cross it.
        std::vector<RouteLevel> levels;
        for (int l = 0; l < routes->levels(); ++l) {
            const RouteGrid* g = routes->level(l);
            auto found = routeLinks_.find(l);
            if (g != nullptr && found == routeLinks_.end()) {
                found = routeLinks_.emplace(l, RouteLinks::build(*g)).first;
            }
            levels.push_back({g, g != nullptr ? &found->second : nullptr});
        }
        std::vector<RouteGoal> onLevel(goals.begin(), goals.end());
        for (RouteGoal& g : onLevel) {
            g.level = level;
        }
        stairs = stairPortals(world_, code_);
        std::vector<RoutePortal> portals;
        for (const StairPortal& p : stairs) {
            RoutePortal r;
            r.x[0] = p.from.x;
            r.y[0] = p.from.y;
            r.level[0] = p.from.level - 1;
            r.x[1] = p.to.x;
            r.y[1] = p.to.y;
            r.level[1] = p.to.level - 1;
            r.twoWay = p.twoWay;
            const auto [refused, cost] = stairCheck(sim, p);
            r.refused = refused;
            r.cost = cost;
            portals.push_back(r);
        }
        plan = planRoute(levels, portals, start, onLevel, aimX, aimY, doors, routeJitter_, random, intersects);
    }
    if (!plan) {
        route.failure = kPathNotFound;
        return route;
    }
    for (std::size_t i = 0; i < plan->walk.size(); ++i) {
        const RouteWaypoint& w = plan->walk[i];
        route.points.emplace_back(w.x, w.y);
        if (w.kind != WaypointKind::LevelEnd || w.portal < 0 || static_cast<std::size_t>(w.portal) >= stairs.size()) {
            continue;
        }
        // The stair's end the walk reaches first is its entry.
        const StairPortal& p = stairs[static_cast<std::size_t>(w.portal)];
        const bool fromEnd = std::hypot(w.x - p.from.x, w.y - p.from.y) <= std::hypot(w.x - p.to.x, w.y - p.to.y);
        const StairPortal::End& entry = fromEnd ? p.from : p.to;
        const StairPortal::End& exit = fromEnd ? p.to : p.from;
        route.stairs.push_back({i, entry.part, exit.part, entry.slot, exit.slot, entry.level, exit.level, entry.facing});
    }
    // A seat goal: the seat, the joint of its slot and the facing there. A bone slot that turns the
    // Sim: the facing of the joint whose spot was reached.
    const RouteGoal& reached = goals[static_cast<std::size_t>(plan->goal)];
    route.goalObject = reached.object;
    route.goalToken = reached.token;
    if (reached.facing) {
        route.facing = static_cast<std::int16_t>(*reached.facing);
        return route;
    }
    // A swept slot's spot faces as the slot says (see slotGoalFacing): a social's slot faces the
    // other Sim, as both Sims stand in the game.
    if (swept) {
        if (const auto facing = slotGoalFacing(base, slot, reached.x, reached.y)) {
            route.facing = static_cast<std::int16_t>(*facing);
        }
        return route;
    }
    // A bone slot's spot faces as its bone turns, whatever the slot's flag bit 12 says (that flag is
    // only Snap's): a Sim at a fridge ends its walk facing it.
    for (const auto& joint : joints) {
        if (!joint) {
            continue;
        }
        const auto target = routingSlotTarget(owner, slot, joint);
        if (target && std::fabs(static_cast<float>(target->x) / 65536.0f - reached.x) < 1e-3f &&
            std::fabs(static_cast<float>(target->y) / 65536.0f - reached.y) < 1e-3f) {
            route.facing = isBoneSlot(slot) ? std::optional<std::int16_t>(boneSlotFacing(owner, *joint)) : target->facing;
            break;
        }
    }
    return route;
}

void LotSimulation::findHousehold(const content::Neighborhood& hood, const std::filesystem::path& lot) {
    const auto number = content::lotNumber(lot);
    if (!number) {
        return;
    }
    for (const auto& [instance, family] : hood.families) {
        if (family.lot != *number || family.members.empty()) {
            continue;
        }
        for (const std::uint32_t guid : family.members) {
            const SimObject* sim = world_.nextAfter(0, [&](const SimObject& o) { return o.isPerson() && o.definition.guid() == guid; });
            if (sim != nullptr) {
                household_.push_back(sim->id);
            }
        }
        return;
    }
}

std::vector<std::uint8_t> LotSimulation::simImage(std::int16_t sim, std::uint32_t instance) {
    const SimObject* object = world_.find(sim);
    const auto* image = object ? index_.find({types::kImage, object->group, instance}) : nullptr;
    return image ? index_.read(*image) : std::vector<std::uint8_t>{};
}

std::optional<std::uint32_t> LotSimulation::characterGroup(std::int16_t neighbor) {
    const SavedNeighbor* n = world_.neighbor(neighbor);
    const ObjectType* type = n ? code_.objectType(n->guid) : nullptr;
    return type ? std::optional(type->group) : std::nullopt;
}

std::optional<std::filesystem::path> LotSimulation::characterFile(std::int16_t neighbor) const {
    const SavedNeighbor* n = world_.neighbor(neighbor);
    const auto it = n ? characterFiles_.find(n->guid) : characterFiles_.end();
    return it != characterFiles_.end() ? std::optional<std::filesystem::path>(it->second) : std::nullopt;
}

void LotSimulation::findCharacterFiles() {
    characterFiles_.clear();
    std::vector<content::ResourceIndex::Resource> definitions;
    for (const auto& r : index_.resources()) {
        if (r.entry.key.type == types::kObjectDefinition && index_.packagePath(r.package).parent_path().filename() == "Characters") {
            definitions.push_back(r);
        }
    }
    for (const auto& r : definitions) {
        try {
            characterFiles_.emplace(objd::parse(index_.read(r)).guid(), index_.packagePath(r.package));
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("Sims: a character's object definition that can't be read is left out: ") + ex.what());
        }
    }
}

// What the game does with each object once the lot is loaded: it runs the object's Load function, and
// an object whose type changed since the save is reset, runs Init and then its Reset function (a Sim
// gets the Sim reset and runs only Reset). The parts of a multi-tile object are all reset before any
// of them runs Init. Object functions run to the end there and then, each within the usual step limit.
void LotSimulation::startObjects() {
    constexpr std::uint16_t kLoad = 2, kInit = 0, kReset = 0x2B;  // OBJf entries
    const std::set<std::int16_t> changed(report_.changedType.begin(), report_.changedType.end());
    std::vector<std::int16_t> ids;
    world_.nextAfter(0, [&](const SimObject& o) {
        ids.push_back(o.id);
        return false;
    });
    // Runs object function `entry` of object `id`, if it still exists and has one. A function whose
    // tree doesn't exist is skipped without an error, as the game does (the Genie controller's Load
    // names one no pack ships).
    auto runFunction = [&](std::int16_t id, std::uint16_t entry) {
        SimObject* me = world_.find(id);
        const ObjectType* type = me ? code_.objectType(me->definition.guid()) : nullptr;
        const auto* functions = type ? code_.objectFunctions(*type) : nullptr;
        if (functions == nullptr || entry >= functions->size() || (*functions)[entry].action == 0 ||
            !vm_.hasTree(type->group, (*functions)[entry].action)) {
            return;
        }
        note(vm_.runOnce(*me, type->group, (*functions)[entry].action), id);
    };
    // Init and Reset after a reset, then the two placement fields Init may not set.
    auto finishReset = [&](std::int16_t id) {
        runFunction(id, kInit);
        SimObject* me = world_.find(id);
        if (const ObjectType* type = me ? code_.objectType(me->definition.guid()) : nullptr) {
            finishSingleTileReset(*me, *type);
        }
        runFunction(id, kReset);
    };
    std::vector<std::int16_t> resetParts;  // of multi-tile objects
    for (const std::int16_t id : ids) {
        runFunction(id, kLoad);
        SimObject* object = world_.find(id);
        const ObjectType* type = object ? code_.objectType(object->definition.guid()) : nullptr;
        if (type == nullptr || !changed.contains(id)) {
            continue;
        }
        if (object->isPerson()) {
            restartSim(*object, *type);
            runFunction(id, kReset);
            continue;
        }
        resetObject(*object, *type, code_);
        ++resets_;
        if (isMultiTilePart(*type)) {
            resetParts.push_back(id);
        } else {
            finishReset(id);
        }
    }
    for (const std::int16_t id : resetParts) {
        finishReset(id);
    }
    world_.purge();
}

// The Sim reset, then the Sim's Cleanup function run there and then.
void LotSimulation::restartSim(SimObject& sim, const ObjectType& type) {
    constexpr std::uint16_t kCleanup = 3;  // OBJf entry
    resetSim(sim, type, code_);
    ++resets_;
    const auto* functions = code_.objectFunctions(type);
    if (functions != nullptr && kCleanup < functions->size() && (*functions)[kCleanup].action != 0) {
        note(vm_.runOnce(sim, type.group, (*functions)[kCleanup].action), sim.id);
    }
}

// A line in the log for each interaction a Sim starts, what a tester's Sim was doing before a problem.
void LotSimulation::logInteractionStart(const SimObject& sim, const QueuedInteraction& started) {
    auto& last = loggedStarts_[sim.id];
    if (last == std::pair{started.object, started.index} || loggedStartCount_ > kMaxLoggedStarts) {
        return;
    }
    last = {started.object, started.index};
    if (++loggedStartCount_ > kMaxLoggedStarts) {
        diag::info("simulation: no more interaction starts are logged for this lot");
        return;
    }
    const SimObject* object = world_.find(started.object);
    std::string name = object != nullptr ? interactionName(code_, *object, started.index) : "?";
    // A line a check tree offered goes by its own name.
    if (!started.name.empty() && started.name != name) {
        name = std::format("{}\" offered by \"{}", started.name, name);
    }
    const char* source = started.source == QueuedInteraction::Source::Player     ? "player"
                         : started.source == QueuedInteraction::Source::Autonomy ? "autonomy"
                                                                                 : "script";
    diag::info(std::format("simulation: Sim {} starts \"{}\" on {} {} ({}, tick {})", sim.id, name, object != nullptr ? object->definition.name : "?",
                           started.object, source, ticks_));
}

// Records a run that ended in a problem. An object that removed itself is not one: the game just
// stops its thread there.
void LotSimulation::note(const RunResult& r, std::int16_t object) {
    if (r.outcome == Outcome::True || r.outcome == Outcome::False || r.outcome == Outcome::Sleeping ||
        r.outcome == Outcome::MeRemoved) {
        return;
    }
    std::string why = outcomeName(r.outcome);
    why += r.reason.empty() ? "" : ": " + r.reason;
    why += r.detail.empty() ? "" : " (" + r.detail + ")";
    Stop& stop = stops_[why];
    if (stop.count++ == 0) {
        stop.first = r;
        stop.object = object;
        stop.tick = ticks_;
        // The first of each kind goes to the log too, with the tree and node it stopped in, at most
        // kMaxLoggedStops so a broken lot can't flood it.
        if (stops_.size() <= kMaxLoggedStops) {
            const auto bhav = code_.behavior(r.group, r.instance);
            diag::warning(std::format("simulation: object {} at tick {}: {}, in {} (0x{:08X} 0x{:04X}) at {}", object, ticks_, why,
                                      bhav ? bhav->name : std::string("?"), r.group, r.instance, r.pc));
        }
    }
}

// One object's turn in a tick: its thread runs until it waits.
void LotSimulation::stepObject(std::int16_t id) {
    SimObject* object = world_.find(id);
    if (object != nullptr && object->failedRun) {
        RunResult failed;
        failed.outcome = Outcome::Error;
        failed.reason = "a tree run on it from another object failed";
        failed.detail = *object->failedRun;
        note(failed, id);
        object->failedRun.reset();
        if (const ObjectType* type = code_.objectType(object->definition.guid())) {
            resetAfterFailure(*object, *type);
            return;
        }
        world_.releaseUses(object->thread.frames);
        object->thread.frames.clear();
    }
    if (object == nullptr || !object->thread.isRunning()) {
        return;
    }
    object->inViewStale = true;
    // A thread that waits n ticks runs again on the n-th tick after: a Sleep of 1 goes on next tick.
    if (object->thread.sleepTicks > 0 && --object->thread.sleepTicks > 0) {
        motiveUpdate_.tick(*object, static_cast<std::int64_t>(ticks_));
        return;
    }
    const RunResult r = vm_.resume(*object);
    if (r.outcome != Outcome::Sleeping) {
        note(r, id);
    }
    if (r.outcome != Outcome::True && r.outcome != Outcome::False) {
        if (SimObject* ran = world_.find(id); ran != nullptr && !ran->removed) {
            motiveUpdate_.tick(*ran, static_cast<std::int64_t>(ticks_));
        }
    }
    // An object whose script fails (an error, a loop that never yields, or a primitive we don't run
    // yet) is reset and starts over.
    if (r.outcome != Outcome::Error && r.outcome != Outcome::StepLimit && r.outcome != Outcome::Unsupported) {
        return;
    }
    SimObject* failed = world_.find(id);  // the run may have created or removed objects
    const ObjectType* type = failed != nullptr && !failed->removed ? code_.objectType(failed->definition.guid()) : nullptr;
    if (type != nullptr) {
        resetAfterFailure(*failed, *type);
    }
}

// The game's reset after a script error. An object with a Cleanup function runs it and starts Main
// again with its data kept. One without gets the full reset a new object gets: data from its OBJD,
// Main, Init and the two placement fields. Only the failing part of a multi-tile object is reset.
// Before removed objects are freed, every Sim forgets each of them, as the game's delete tells them.
void LotSimulation::forgetRemoved() {
    const std::vector<std::int16_t> removed = world_.removedIds();
    if (removed.empty()) {
        return;
    }
    std::vector<std::int16_t> sims;
    world_.nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& o) {
        if (o.isPerson()) {
            sims.push_back(o.id);
        }
        return false;
    });
    for (const std::int16_t gone : removed) {
        for (const std::int16_t id : sims) {
            if (SimObject* sim = world_.find(id); sim != nullptr && sim->id != gone) {
                forget(*sim, gone);
            }
        }
    }
}

void LotSimulation::forget(SimObject& sim, std::int16_t removed) {
    if (forgetObject(sim, removed)) {
        vm_.resetSim(sim);
    }
}

void LotSimulation::resetAfterFailure(SimObject& object, const ObjectType& type) {
    constexpr std::uint16_t kInit = 0, kCleanup = 3;  // OBJf entries
    world_.releaseUses(object.thread.frames);
    object.thread.frames.clear();
    if (object.isPerson()) {
        restartSim(object, type);
        return;
    }
    ++resets_;
    const std::int16_t id = object.id;
    const auto* functions = code_.objectFunctions(type);
    auto tree = [&](std::uint16_t entry) -> std::uint16_t {
        return functions != nullptr && entry < functions->size() ? (*functions)[entry].action : 0;
    };
    if (const std::uint16_t cleanup = tree(kCleanup); cleanup != 0) {
        note(vm_.runOnce(object, type.group, cleanup), id);
        if (SimObject* me = world_.find(id); me != nullptr && !me->removed) {
            restartMain(*me, type, code_);
        }
        return;
    }
    resetObject(object, type, code_);
    if (const std::uint16_t init = tree(kInit); init != 0) {
        note(vm_.runOnce(object, type.group, init), id);
    }
    if (SimObject* me = world_.find(id); me != nullptr && !me->removed) {
        finishSingleTileReset(*me, type);
    }
}

// The object's timers count down after its turn, as the game's scheduler runs them. One that runs
// out runs its tree at once on top of whatever the object is doing.
void LotSimulation::runTimers(std::int16_t id) {
    SimObject* owner = world_.find(id);
    if (owner == nullptr || owner->removed || owner->timers.empty()) {
        return;
    }
    const ObjectType* type = code_.objectType(owner->definition.guid());
    // Timers a fired tree sets wait for the next tick, as the game counts them before the loop. The
    // tree may also remove timers, so each is looked up again by its id.
    std::vector<std::uint16_t> ids;
    for (const auto& t : owner->timers) {
        ids.push_back(t.id);
    }
    for (const std::uint16_t timerId : ids) {
        auto& timers = owner->timers;
        const auto byId = [&timers](std::uint16_t wanted) {
            return std::find_if(timers.begin(), timers.end(), [&](const ObjectTimer& t) { return t.id == wanted; });
        };
        const auto timer = byId(timerId);
        if (timer == timers.end() || --timer->countdown >= 1) {
            continue;
        }
        const ObjectTimer fired = *timer;
        bool again = false;
        if (type != nullptr) {
            const RunResult r = vm_.runEvent(*owner, type->group, fired.tree, fired.setter, fired.args);
            note(r, id);
            again = r.outcome == Outcome::True;
        }
        owner = world_.find(id);  // the tree may have removed objects, this one too
        if (owner == nullptr || owner->removed) {
            return;
        }
        auto& after = owner->timers;
        const auto at = std::find_if(after.begin(), after.end(), [&](const ObjectTimer& t) { return t.id == fired.id; });
        if (at == after.end()) {
            continue;  // the tree removed it itself
        }
        if (fired.repeats && again) {
            at->countdown = fired.interval;
        } else {
            after.erase(at);
        }
    }
}

void LotSimulation::tick() {
    diag::Timer timer("simulation tick");
    world_.advanceClock(kNormalTickMs);
    std::vector<std::int16_t> ids;
    world_.nextAfter(0, [&](const SimObject& o) {
        ids.push_back(o.id);
        return false;
    });
    for (const std::int16_t id : ids) {
        tellOverlayEvents(world_, vm_, id, [&](const RunResult& r) { note(r, id); }, world_.clockMs());
        tellAnimateSimEvents(world_, vm_, id, [&](const RunResult& r) { note(r, id); }, world_.clockMs());
    }
    // Ids run in order up to the highest one held as the pass starts. An object made during the pass
    // runs in it when it takes a free id after the one running.
    const std::int16_t last = world_.highestId();
    for (int i = 1; i <= last; ++i) {
        const auto id = static_cast<std::int16_t>(i);
        if (world_.findIncludingRemoved(id) == nullptr) {
            continue;
        }
        // The destinations a Sim made to make way go once nothing uses them, checked as its tick
        // starts, before its scripts run. All of them go once the Sim is gone.
        if (SimObject* o = world_.find(id); o != nullptr && !o->destinations.empty()) {
            cleanUpDestinations(world_, *o, false);
        }
        stepObject(id);
        runTimers(id);
        if (SimObject* o = world_.findIncludingRemoved(id); o != nullptr && o->removed && !o->destinations.empty()) {
            cleanUpDestinations(world_, *o, true);
        }
    }
    advanceClock(vm_.globals);
    forgetRemoved();
    world_.purge();
    world_.countTick();
    ++ticks_;
}

void LotSimulation::tellAnimationEvents(double nowMs) {
    std::vector<std::int16_t> ids;
    world_.nextAfter(0, [&](const SimObject& o) {
        ids.push_back(o.id);
        return false;
    });
    for (const std::int16_t id : ids) {
        tellOverlayEvents(world_, vm_, id, [&](const RunResult& r) { note(r, id); }, nowMs);
        tellAnimateSimEvents(world_, vm_, id, [&](const RunResult& r) { note(r, id); }, nowMs);
    }
}

void LotSimulation::tellLookStates(std::int16_t id, std::span<const LookController::Change> changes) {
    ts2::sim::tellLookStates(world_, vm_, id, changes, [&](const RunResult& r) { note(r, id); });
    if (SimObject* o = world_.find(id)) {
        noteLook(*o);
    }
}

void LotSimulation::markOnScreen(std::span<const std::int16_t> objects) {
    constexpr std::size_t kOnScreenMinute = 102;
    const auto& g = vm_.globals;
    const auto minute = static_cast<std::int16_t>(g[global::kHour] * 60 + g[global::kMinute] + 1);
    for (const std::int16_t id : objects) {
        if (SimObject* o = world_.find(id)) {
            o->data[kOnScreenMinute] = minute;
        }
    }
}

} // namespace ts2::sim
