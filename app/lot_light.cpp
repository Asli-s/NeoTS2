#include "lot_light.hpp"

#include "content/covering_colors.hpp"
#include "content/lighting_state.hpp"
#include "content/lot.hpp"
#include "diag/log.hpp"
#include "lot_view.hpp"
#include "scene/lot_light_maps.hpp"
#include "scene/lot_light_objects.hpp"
#include "scene/lot_scene.hpp"
#include "scene/lot_lighting.hpp"
#include "scene/lot_shadow.hpp"
#include "scene/lot_skirt.hpp"
#include "scene/object_lights.hpp"
#include "scene/object_sun.hpp"
#include "sim/clock.hpp"
#include "sim/rooms.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace app {

namespace {

// The largest lot side (tiles) the per-tile caches below are kept for, as the shadow allows.
constexpr int kMaxCachedSide = 256;

// A scene's light-map images in the order putLotLight lays them out: floors, the floors'
// incidence maps, walls, the walls' incidence map.
std::vector<int> lightMapImages(const ts2::scene::LightMaps& maps) {
    std::vector<int> out(maps.floors.begin(), maps.floors.end());
    out.insert(out.end(), maps.floorIncidence.begin(), maps.floorIncidence.end());
    out.push_back(maps.walls);
    out.push_back(maps.wallIncidence);
    return out;
}

} // namespace

struct LotLightBase {
    ts2::content::Lot lot;  // all of it: levels count from the ground storey, a pool's storey is -1
    // From the ground storey up, for the lamps, windows and doors (none stand below it).
    ts2::content::Lot fromGround;
    std::filesystem::path package;
    std::shared_ptr<const ts2::sim::RoomMap> rooms;
    std::function<std::int16_t(int level, float x, float y)> roomAt;
    std::set<std::uint32_t> clearWalls;  // wall styles that let light through (see makeBase)
    std::unique_ptr<ts2::content::CoveringColors> colors;
    ts2::scene::lotshadow::Inputs shadowIn;  // reads `lot`
    // Whether each ground tile is bare terrain, and the ground's tile corners from a tile before the
    // lot to a tile past it: asked for every texel, so worked out once.
    std::vector<std::uint8_t> bareTiles;
    std::vector<ts2::scene::lotshadow::Corners> groundCorners;
    int cornersWide = 0;
    // The lot scene's light maps before any light was put in them (fill keeps what it does not reach).
    ts2::scene::Scene maps;
};

struct LotLight {
    std::shared_ptr<LotLightBase> base;
    std::string state;
    ts2::scene::lotlighting::Settings settings;
    ts2::scene::lotlightobjects::Found found;
    ts2::scene::ObjectLighting outdoor;
    std::vector<ts2::scene::Image> sky;  // the skybox's six faces
    std::array<float, 3> environment{1, 1, 1};
    float zone = 1, rangeCutoff = 0.275f;
    // Worked out:
    ts2::scene::lotlighting::Result light;
    std::shared_ptr<const ts2::scene::lotshadow::Grid> shadow;
    ts2::scene::Scene maps;  // the base's light maps with this light in them
    bool done = false;
};

namespace {

// Works the light out and fills the light maps of `target` (the lot scene, or a copy of its maps).
void workOut(LotLight& l, ts2::scene::Scene& target) {
    using namespace ts2::scene;
    const auto start = std::chrono::steady_clock::now();
    auto& base = *l.base;
    const auto& lot = base.lot;
    const auto& rooms = *base.rooms;
    lotlighting::Inputs in;
    in.lot = &lot;
    in.lamps = l.found.lamps;
    in.portals = l.found.portals;
    // Fences (coverings past 0x10000) and wall styles that don't block light let it through.
    in.blocksLight = [&base](std::uint32_t covering) { return covering < 0x10000 && !base.clearWalls.contains(covering); };
    in.roomAt = base.roomAt;
    in.quarterRoom = [&](int level, int x, int y, int bit) {
        // The room map's quarters run top (-y), right, bottom, left.
        // The lighting's quarters run west, north, east, south.
        const int quarter = bit == 3 ? 0 : bit == 2 ? 1 : bit == 1 ? 2 : 3;
        return static_cast<std::int16_t>(rooms.quarterRoom(level, x, y, quarter));
    };
    in.covering = [&](bool floor, const std::string& name) {
        return base.colors->color(floor ? ts2::content::CoveringKind::Floor : ts2::content::CoveringKind::Wall, name);
    };
    for (const auto& [id, info] : rooms.rooms()) {
        in.roomCount = std::max(in.roomCount, id + 1);
    }
    l.light = lotlighting::compute(in, l.settings);
    if (!l.light.complete) {
        ts2::diag::warning("lot light: a pass stopped at its safety limit");
    }
    // The buildings' sun shadow over the lot.
    const auto& shadowIn = base.shadowIn;
    const auto& sunDir = l.settings.outdoor.lights.empty() ? std::array<float, 3>{0, 0, 1} : l.settings.outdoor.lights.front().direction;
    l.shadow = std::make_shared<const lotshadow::Grid>(lotshadow::build(shadowIn, sunDir));
    const auto floorGrid = lot.grids.find(ts2::content::kFloorGridInstance);
    const int ground = ts2::content::groundGridLevel(lot);
    auto bareGround = [&](int x, int y) {
        if (floorGrid == lot.grids.end()) return true;
        for (int q = 0; q < 4; ++q) {
            if (floorGrid->second.contains(ground, x, y) && floorGrid->second.i16(ground, x, y, q) != 0) return false;
        }
        return true;
    };
    lotlightmaps::Sunlight sunlight;
    sunlight.shadow = l.shadow.get();
    sunlight.lot = &shadowIn;
    sunlight.zone = l.zone;
    sunlight.terrain = [&](int x, int y) {
        if (base.bareTiles.empty() || x < 0 || y < 0 || x >= shadowIn.width || y >= shadowIn.height) return bareGround(x, y);
        return base.bareTiles[static_cast<std::size_t>(y) * static_cast<std::size_t>(shadowIn.width) + static_cast<std::size_t>(x)] != 0;
    };
    auto groundAt = [&](float x, float y) {
        const float fx = std::floor(x), fy = std::floor(y);
        if (!base.groundCorners.empty() && fx >= -1 && fy >= -1 && fx <= static_cast<float>(shadowIn.width) && fy <= static_cast<float>(shadowIn.height)) {
            const auto i = static_cast<std::size_t>(fy + 1) * static_cast<std::size_t>(base.cornersWide) + static_cast<std::size_t>(fx + 1);
            return base.groundCorners[i].between(x - fx, y - fy);
        }
        return lotshadow::elevation(shadowIn, x, y, 0);
    };
    sunlight.terrainNormal = [&](float x, float y) {
        const float sx = groundAt(x + 0.5f, y) - groundAt(x - 0.5f, y);
        const float sy = groundAt(x, y + 0.5f) - groundAt(x, y - 0.5f);
        const float len = std::sqrt(sx * sx + sy * sy + 1);
        return std::array<float, 3>{-sx / len, -sy / len, 1 / len};
    };
    lotlightmaps::fill(target, l.light, l.settings, l.shadow->width > 0 ? &sunlight : nullptr);
    l.done = true;
    ts2::diag::info(std::format("lot light ({}): {} lamps, {} windows and doors, {} rooms in {:.1f} ms", l.state, l.found.lamps.size(),
                                l.found.portals.size(), l.light.rooms.size(),
                                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()));
}

// What lighting the lot again needs, with the scene's light maps as they are before any light.
std::shared_ptr<LotLightBase> makeBase(ts2::scene::Assets& assets, const ts2::scene::Scene& scene, const ts2::content::Lot& saved,
                                       const std::filesystem::path& lotPackage) {
    using namespace ts2::scene;
    auto base = std::make_shared<LotLightBase>();
    base->lot = saved;
    base->fromGround = ts2::content::fromGroundStorey(saved);
    const auto& lot = base->lot;
    const auto layer = lot.layerGraphs.find(ts2::content::kLayerGraphInstances[0]);
    if (!scene.lightMaps || layer == lot.layerGraphs.end() || !lot.roomGraph) {
        return nullptr;
    }
    base->package = lotPackage;
    // Styles walls.txt marks blocksLight false, but the pool's edge (type 0x1D) blocks light anyway,
    // as the game's own wall test has it.
    constexpr std::uint32_t kPoolEdge = 0x1D;
    for (const auto& [id, style] : assets.buildScripts().wallStyles) {
        if (!style.blocksLight && id != kPoolEdge) {
            base->clearWalls.insert(id);
        }
    }
    base->rooms = std::make_shared<const ts2::sim::RoomMap>(layer->second);
    base->roomAt = [rooms = base->rooms](int level, float x, float y) {
        const auto r = rooms->roomAt(level, x, y);
        return static_cast<std::int16_t>(r < 0 ? 0 : r);
    };
    // The user folder's build-mode thumbnails give the coverings' colours (Neighborhoods/<hood>/Lots/<lot>).
    const auto userFolder = lotPackage.parent_path().parent_path().parent_path().parent_path();
    base->colors = std::make_unique<ts2::content::CoveringColors>(userFolder / "Thumbnails" / "BuildModeThumbnails.package");
    base->shadowIn = lotshadow::fromLot(lot, [&](std::uint32_t covering) { return assets.fence(covering) != nullptr; });
    const auto& shadowIn = base->shadowIn;
    const bool small = shadowIn.width > 0 && shadowIn.height > 0 && shadowIn.width <= kMaxCachedSide && shadowIn.height <= kMaxCachedSide;
    const auto floorGrid = lot.grids.find(ts2::content::kFloorGridInstance);
    const int ground = ts2::content::groundGridLevel(lot);
    if (small) {
        base->bareTiles.reserve(static_cast<std::size_t>(shadowIn.width) * static_cast<std::size_t>(shadowIn.height));
        for (int y = 0; y < shadowIn.height; ++y) {
            for (int x = 0; x < shadowIn.width; ++x) {
                bool bare = true;
                for (int q = 0; q < 4 && bare && floorGrid != lot.grids.end(); ++q) {
                    bare = !(floorGrid->second.contains(ground, x, y) && floorGrid->second.i16(ground, x, y, q) != 0);
                }
                base->bareTiles.push_back(bare ? 1 : 0);
            }
        }
    }
    if (small && shadowIn.corner && shadowIn.levels > 0) {
        base->cornersWide = shadowIn.width + 2;
        base->groundCorners.reserve(static_cast<std::size_t>(base->cornersWide) * static_cast<std::size_t>(shadowIn.height + 2));
        for (int y = -1; y <= shadowIn.height; ++y) {
            for (int x = -1; x <= shadowIn.width; ++x) base->groundCorners.push_back(lotshadow::cornersOf(shadowIn, x, y, 0));
        }
    }
    // The light maps alone, numbered from 0 in the order lightMapImages gives.
    LightMaps maps = *scene.lightMaps;
    int next = 0;
    auto take = [&](int& index) {
        if (index < 0 || static_cast<std::size_t>(index) >= scene.images.size()) {
            index = -1;
            return;
        }
        base->maps.images.push_back(scene.images[static_cast<std::size_t>(index)]);
        index = next++;
    };
    for (auto& f : maps.floors) take(f);
    for (auto& f : maps.floorIncidence) take(f);
    take(maps.walls);
    take(maps.wallIncidence);
    base->maps.lightMaps = std::move(maps);
    return base;
}

} // namespace

std::string savedLightingState(const ts2::content::Lighting& lighting, const ts2::content::Lot& saved) {
    // A lot saved without a clock starts at 8 am, as the game's simulator does.
    const int hour = saved.globals.empty() ? 8 : saved.globals[ts2::sim::global::kHour];
    return ts2::content::lotLightingState(lighting, static_cast<int>(ts2::sim::timeOfDay(hour)));
}

std::shared_ptr<LotLight> gatherLotLight(ts2::scene::Assets& assets, std::shared_ptr<LotLightBase> base, const std::string& stateName) {
    using namespace ts2::scene;
    if (!base) {
        return nullptr;
    }
    const auto& lighting = assets.lighting();
    auto out = std::make_shared<LotLight>();
    const auto* state = ts2::content::findState(lighting, stateName, &out->state);
    auto settings = state ? lotlighting::settingsFor(lighting, out->state) : std::nullopt;
    if (!settings) {
        return nullptr;
    }
    out->settings = std::move(*settings);
    lotlightobjects::Queries queries;
    queries.roomAt = base->roomAt;
    queries.strength = [](std::uint32_t) { return 0.0f; };  // lamps off until the lot's scripts switch them
    out->found = lotlightobjects::gather(assets, base->fromGround, base->package, *state, queries);
    out->outdoor = outdoorLighting(*state);
    out->environment = state->environment;
    out->sky = assets.textureFaces(ts2::content::lotSkybox(ts2::content::stateTime(out->state)));
    out->zone = lighting.number("hmSunLightTransitionZone", 1);
    out->rangeCutoff = lighting.number("lightRangeCutoff", 0.275f);
    out->base = std::move(base);
    return out;
}

void workOutLotLight(LotLight& light) {
    light.maps = light.base->maps;
    workOut(light, light.maps);
}

std::shared_ptr<LotLightBase> lightLot(ts2::scene::Assets& assets, ts2::scene::Scene& scene, const ts2::content::Lot& saved,
                                       const std::filesystem::path& lotPackage, const std::string& state) {
    using namespace ts2::scene;
    auto base = makeBase(assets, scene, saved, lotPackage);
    const auto light = gatherLotLight(assets, base, state);
    if (!light) {
        return nullptr;
    }
    workOut(*light, scene);
    scene.objectLighting = light->outdoor;
    if (!light->sky.empty()) {
        scene.skybox = light->sky;
    }
    objectsun::shade(scene, *light->shadow, light->zone);
    objectlights::light(scene, light->light.rooms, light->light.portals, light->rangeCutoff, base->roomAt, ts2::content::groundGridLevel(saved));
    scene.thingSun = objectsun::thingSun(light->shadow, light->zone);
    scene.thingLights = objectlights::thingLights(light->light.rooms, light->light.portals, light->rangeCutoff, base->roomAt);
    return base;
}

PutLight putLotLight(ts2::scene::Scene& scene, const LotLight& light, const ts2::scene::Transform& place) {
    using namespace ts2::scene;
    PutLight out;
    if (!light.done || !scene.lightMaps || !light.maps.lightMaps) {
        return out;
    }
    const auto to = lightMapImages(*scene.lightMaps);
    const auto from = lightMapImages(*light.maps.lightMaps);
    for (std::size_t i = 0; i < to.size() && i < from.size(); ++i) {
        const int t = to[i], f = from[i];
        if (t < 0 || f < 0 || static_cast<std::size_t>(t) >= scene.images.size() || static_cast<std::size_t>(f) >= light.maps.images.size()) {
            continue;
        }
        auto& image = scene.images[static_cast<std::size_t>(t)];
        const auto& fresh = light.maps.images[static_cast<std::size_t>(f)];
        if (image.width == fresh.width && image.height == fresh.height) {
            image = fresh;
            out.images.push_back(t);
        }
    }
    if (!light.sky.empty()) {
        scene.skybox = light.sky;
        out.sky = true;
    }
    scene.thingSun = objectsun::thingSun(light.shadow, light.zone);
    scene.thingLights = [lights = objectlights::thingLights(light.light.rooms, light.light.portals, light.rangeCutoff, light.base->roomAt),
                         place](const Mesh& mesh, const Transform& inLot, int storey) {
        auto sets = lights(mesh, inLot, storey);
        for (auto& set : sets) {
            placeLights(set, place);
        }
        return sets;
    };
    // Light sets only ever grow. Past this a door or window keeps the light it has.
    constexpr std::size_t kMaxLightSets = 4096;
    auto setOf = [&](ObjectLighting set) {
        placeLights(set, place);
        const auto same = std::find(scene.lightSets.begin(), scene.lightSets.end(), set);
        if (same != scene.lightSets.end()) {
            return std::optional<int>(static_cast<int>(same - scene.lightSets.begin()));
        }
        if (scene.lightSets.size() >= kMaxLightSets) {
            return std::optional<int>();
        }
        scene.lightSets.push_back(std::move(set));
        return std::optional<int>(static_cast<int>(scene.lightSets.size() - 1));
    };
    std::vector<std::uint8_t> scripted(scene.instances.size(), 0);
    for (const auto& placed : scene.placedObjects) {
        if (placed.instance < scripted.size()) {
            scripted[placed.instance] = 1;
        }
    }
    const auto toLot = place.rigidInverse();
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        auto& instance = scene.instances[i];
        if (instance.part != Part::Object && !instance.litOutdoors) {
            continue;
        }
        Instance inLot = instance;
        inLot.transform = toLot * instance.transform;
        const int storey = instance.level - scene.groundLevel;
        bool relit = false;
        if (!scripted[i] && !instance.sun.empty() && scene.thingSun) {
            if (auto sun = scene.thingSun(scene, inLot); sun != instance.sun) {
                instance.sun = std::move(sun);
                relit = true;
            }
        }
        if (scripted[i] && instance.lightGroup == LightGroup::Portals) {
            std::vector<int> parts;
            bool full = false;
            for (auto& part : objectlights::portalLights(scene, inLot, storey, light.light.rooms, light.light.portals, light.rangeCutoff,
                                                         light.base->roomAt)) {
                const auto set = part ? setOf(std::move(*part)) : std::optional<int>(-1);
                full = full || !set;
                parts.push_back(set.value_or(-1));
            }
            if (!full && parts != instance.partLightSets) {
                instance.partLightSets = std::move(parts);
                relit = true;
            }
        }
        if (relit) {
            out.instances.push_back(i);
        }
    }
    for (const std::size_t i : lightLotSkirt(scene, ts2::content::stateTime(light.state) != ts2::content::StateTime::Night)) {
        out.instances.push_back(i);
    }
    return out;
}

ts2::scene::ObjectLighting outdoorLotLight(const LotLight& light, const ts2::scene::Transform& place) {
    auto out = light.outdoor;
    placeLights(out, place);
    return out;
}

const std::string& lotLightState(const LotLight& light) { return light.state; }

const std::array<float, 3>& lotLightEnvironment(const LotLight& light) { return light.environment; }

} // namespace app
