#include "lot_light.hpp"

#include "content/covering_colors.hpp"
#include "content/lot.hpp"
#include "diag/log.hpp"
#include "scene/lot_light_maps.hpp"
#include "scene/lot_light_objects.hpp"
#include "scene/lot_lighting.hpp"
#include "scene/lot_shadow.hpp"
#include "sim/rooms.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>

namespace app {

bool lightLot(ts2::scene::Assets& assets, ts2::scene::Scene& scene, const std::filesystem::path& lotPackage) {
    using namespace ts2::scene;
    if (!scene.lightMaps) {
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto lot = ts2::content::loadLot(lotPackage);
    const auto layer = lot.layerGraphs.find(ts2::content::kLayerGraphInstances[0]);
    const auto& lighting = assets.lighting();
    const auto state = lighting.states.find("day");
    const auto settings = lotlighting::settingsFor(lighting, "day");
    if (layer == lot.layerGraphs.end() || !lot.roomGraph || state == lighting.states.end() || !settings) {
        return false;
    }
    const ts2::sim::RoomMap rooms(layer->second);
    auto roomAt = [&](int level, float x, float y) {
        const auto r = rooms.roomAt(level, x, y);
        return static_cast<std::int16_t>(r < 0 ? 0 : r);
    };

    lotlightobjects::Queries queries;
    queries.roomAt = roomAt;
    queries.strength = [](std::uint32_t) { return 0.0f; };  // lamps off until the lot's scripts switch them
    const auto found = lotlightobjects::gather(assets, lot, lotPackage, state->second, queries);

    // The user folder's build-mode thumbnails give the coverings' colours (Neighborhoods/<hood>/Lots/<lot>).
    const auto userFolder = lotPackage.parent_path().parent_path().parent_path().parent_path();
    ts2::content::CoveringColors colors(userFolder / "Thumbnails" / "BuildModeThumbnails.package");

    lotlighting::Inputs in;
    in.lot = &lot;
    in.lamps = found.lamps;
    in.portals = found.portals;
    in.blocksLight = [](std::uint32_t covering) { return covering < 0x10000; };
    in.roomAt = roomAt;
    in.quarterRoom = [&](int level, int x, int y, int bit) {
        // The room map's quarters run top (-y), right, bottom, left; the lighting's west, north, east, south.
        const int quarter = bit == 3 ? 0 : bit == 2 ? 1 : bit == 1 ? 2 : 3;
        return static_cast<std::int16_t>(rooms.quarterRoom(level, x, y, quarter));
    };
    in.covering = [&](bool floor, const std::string& name) {
        return colors.color(floor ? ts2::content::CoveringKind::Floor : ts2::content::CoveringKind::Wall, name);
    };
    for (const auto& [id, info] : rooms.rooms()) {
        in.roomCount = std::max(in.roomCount, id + 1);
    }
    const auto light = lotlighting::compute(in, *settings);
    if (!light.complete) {
        ts2::diag::warning("lot light: a pass stopped at its safety limit");
    }
    // The buildings' sun shadow over the lot.
    const auto floorGrid = lot.grids.find(ts2::content::kFloorGridInstance);
    const auto shadowIn = lotshadow::fromLot(lot, [&](std::uint32_t covering) { return assets.fence(covering) != nullptr; });
    const auto& sunDir = settings->outdoor.lights.empty() ? std::array<float, 3>{0, 0, 1} : settings->outdoor.lights.front().direction;
    const auto shadow = lotshadow::build(shadowIn, sunDir);
    lotlightmaps::Sunlight sunlight;
    sunlight.shadow = &shadow;
    sunlight.lot = &shadowIn;
    sunlight.zone = lighting.number("hmSunLightTransitionZone", 1);
    sunlight.terrain = [&](int x, int y) {
        if (floorGrid == lot.grids.end()) return true;
        for (int q = 0; q < 4; ++q) {
            if (floorGrid->second.contains(0, x, y) && floorGrid->second.i16(0, x, y, q) != 0) return false;
        }
        return true;
    };
    sunlight.terrainNormal = [&](float x, float y) {
        const float sx = lotshadow::elevation(shadowIn, x + 0.5f, y, 0) - lotshadow::elevation(shadowIn, x - 0.5f, y, 0);
        const float sy = lotshadow::elevation(shadowIn, x, y + 0.5f, 0) - lotshadow::elevation(shadowIn, x, y - 0.5f, 0);
        const float len = std::sqrt(sx * sx + sy * sy + 1);
        return std::array<float, 3>{-sx / len, -sy / len, 1 / len};
    };
    lotlightmaps::fill(scene, light, *settings, shadow.width > 0 ? &sunlight : nullptr);
    ts2::diag::info(std::format("lot light: {} lamps, {} windows and doors, {} rooms in {:.1f} ms", found.lamps.size(), found.portals.size(),
                                light.rooms.size(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()));
    return true;
}

} // namespace app
