#include "neighborhood.hpp"

#include "content/cameras.hpp"
#include "content/install.hpp"
#include "content/lighting.hpp"
#include "content/material_scripts.hpp"
#include "content/neighborhood.hpp"
#include "content/nhood_catalog.hpp"
#include "formats/dbpf.hpp"
#include "formats/fami.hpp"
#include "formats/nhood_paint_rules.hpp"
#include "formats/propset.hpp"
#include "orbit.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace app {

namespace {

constexpr std::uint32_t kFamilyType = 0x46414D49;
constexpr std::uint32_t kFirstGameFamily = 0x7FE0;

} // namespace

int lotSkirtMargin(const std::filesystem::path& install) {
    const auto file = ts2::content::newestDataFile(ts2::content::findProducts(install), "Res/Config/globalProps.xml");
    if (!file) {
        return 9;
    }
    std::ifstream in(*file, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto props = ts2::propset::parse(bytes);
    const auto it = props.find("lotSkirtSizeIncrease");
    return it == props.end() ? 9 : std::max(1, std::atoi(it->second.c_str()));
}

std::vector<std::uint32_t> livedInLots(const std::filesystem::path& folder) {
    std::vector<std::uint32_t> out;
    try {
        ts2::dbpf::Package package{ts2::content::neighborhoodPackage(folder)};
        for (const auto& e : package.entries()) {
            if (e.key.type == kFamilyType && e.key.instance < kFirstGameFamily) {
                const auto family = ts2::fami::parse(package.read(e));
                if (family.lot > 0 && !family.members.empty()) {
                    out.push_back(static_cast<std::uint32_t>(family.lot));
                }
            }
        }
    } catch (const std::exception&) {
    }
    return out;
}

ts2::scene::NeighborhoodSceneStats buildNeighborhood(ts2::scene::SceneBuilder& builder, const std::filesystem::path& install,
                                                     const std::filesystem::path& folder) {
    // The paint rules, from the newest product's NeighborhoodTerrain.ini.
    const auto ini = ts2::content::newestDataFile(ts2::content::findProducts(install), "Sys/NeighborhoodTerrain.ini");
    if (!ini) {
        throw std::runtime_error("NeighborhoodTerrain.ini was not found");
    }
    std::ifstream in(*ini, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    // The light map's settings: the "neighbourhood" lighting state and the height map shadow tunables.
    const auto products = ts2::content::findProducts(install);
    const auto lighting = ts2::content::readLighting(products);
    ts2::scene::NeighborhoodLight light;
    light.shadowStrength = lighting.number("hmNHoodShadowStrength", light.shadowStrength);
    light.boxWidening = lighting.number("hmBlobWidthModifier", light.boxWidening);
    light.boxHeightScale = lighting.number("hmBlobHeightModifier", light.boxHeightScale);
    if (const auto state = lighting.states.find("neighbourhood"); state != lighting.states.end()) {
        if (const auto sun = state->second.lights.find("sun"); sun != state->second.lights.end()) {
            const auto& d = sun->second.direction;
            const double len = std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]);
            light.sun = {float(d[0] / len), float(d[1] / len), float(d[2] / len)};
        }
        light.tint = state->second.lightMapTint;
        if (state->second.lightMap) {
            if (auto image = ts2::content::lightingImage(products, *state->second.lightMap)) {
                light.lookup = {image->width, image->height, std::move(image->rgba)};
            }
        }
    }
    if (light.lookup.width == 0) {
        throw std::runtime_error("the neighborhood light map image was not found");
    }
    // How far lot imposters are lifted above the terrain, from the newest Neighborhood.ini.
    float imposterZOffset = 0;
    if (const auto tunables = ts2::content::newestDataFile(products, "Sys/Neighborhood.ini")) {
        std::ifstream file(*tunables);
        for (std::string line; std::getline(file, line);) {
            constexpr std::string_view kKey = "ImposterZOffset=";
            if (line.starts_with(kKey)) {
                imposterZOffset = std::stof(line.substr(kKey.size()));
            }
        }
    }
    auto stats = ts2::scene::addNeighborhood(builder, folder, ts2::nhood::parsePaintRules(text), light,
                                            ts2::content::neighborhoodRoadTextures(products), imposterZOffset,
                                            ts2::content::neighborhoodObjectTypes(products));
    return stats;
}

void neighborhoodCamera(Orbit& orbit, const std::filesystem::path& install, const ts2::scene::NeighborhoodSceneStats& stats) {
    orbit.rig = ts2::content::findCamera(install, "Neighborhood");
    if (!orbit.rig || orbit.rig->levels.empty()) {
        throw std::runtime_error("the game's Neighborhood camera was not found");
    }
    const auto& levels = orbit.rig->levels;
    orbit.maxDistance = levels.front().distance;
    orbit.minDistance = levels.back().distance;
    if (stats.camera) {
        const auto& saved = *stats.camera;
        orbit.target = {saved.target[0], saved.target[1], saved.target[2]};
        orbit.yaw = orbit.yawGoal = saved.yaw;
        orbit.zoomTo(saved.distance);
    } else {
        orbit.target = {static_cast<float>(stats.width) * 5, static_cast<float>(stats.height) * 5, stats.seaLevel};
        orbit.zoomTo(levels[static_cast<std::size_t>(std::clamp(orbit.rig->defaultZoom, 0, static_cast<int>(levels.size()) - 1))].distance);
    }
}

} // namespace app
