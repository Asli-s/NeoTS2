#include "neighborhood.hpp"

#include "content/cameras.hpp"
#include "content/install.hpp"
#include "content/lighting.hpp"
#include "content/lighting_state.hpp"
#include "content/material_scripts.hpp"
#include "content/neighborhood.hpp"
#include "content/nhood_catalog.hpp"
#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/fami.hpp"
#include "formats/nhood_paint_rules.hpp"
#include "formats/propset.hpp"
#include "orbit.hpp"
#include "scene/lot_scene.hpp"
#include "scene/neighborhood_decals.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
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
    } catch (const std::exception& ex) {
        ts2::diag::warnOnce(std::string("neighborhood: the families can't be read, no lot shows as lived in: ") + ex.what());
    }
    return out;
}

void addPlumbobs(ts2::scene::SceneBuilder& builder, ts2::content::EffectsFiles& effects, const std::filesystem::path& folder,
                 const ts2::scene::NeighborhoodSceneStats& stats, ts2::scene::NeighborhoodPlumbobs& plumbobs) {
    constexpr const char* kPlumbob = "fxNeighborhoodPlumbob";
    const auto lived = livedInLots(folder);
    std::vector<std::array<float, 3>> anchors;
    for (const auto& lot : stats.lotFootprints) {
        if (lot.hasImposter && std::find(lived.begin(), lived.end(), lot.id) != lived.end()) {
            anchors.push_back(lot.effectAnchor);
        }
    }
    if (anchors.empty()) {
        return;
    }
    const auto* file = effects.with(kPlumbob);
    if (file == nullptr) {
        ts2::diag::warnOnce("neighborhood: no effects file has the plumbob, lots show none");
        return;
    }
    // The game lights the neighborhood's objects with the "neighbourhoodNoPortals" state's lights,
    // as outdoor objects on a lot are lit (the ground's light map uses "neighbourhood").
    if (const auto* state = ts2::content::findState(builder.assets().lighting(), "neighbourhoodNoPortals")) {
        builder.scene().objectLighting = ts2::scene::outdoorLighting(*state);
    }
    if (plumbobs.add(builder, *file, kPlumbob, anchors) == 0) {
        ts2::diag::warnOnce("neighborhood: the plumbob effect plays nothing that can be shown yet");
    }
}

namespace {

// The effects of the decorations from the catalog's effects section, as each one's model names
// them at its slots, the game's state without snow. Only their ground decals are played so far.
void addDecorationEffects(ts2::scene::SceneBuilder& builder, ts2::content::EffectsFiles& effects,
                          const ts2::scene::NeighborhoodSceneStats& stats, std::optional<std::uint32_t> seed) {
    std::map<std::string, std::vector<ts2::scene::EffectSlot>> slots;
    std::mt19937 random{seed ? *seed : std::random_device{}()};
    for (const auto& decoration : stats.effectDecorations) {
        auto known = slots.find(decoration.model);
        if (known == slots.end()) {
            known = slots.emplace(decoration.model, ts2::scene::effectSlots(builder.assets(), decoration.model)).first;
        }
        const float turn = decoration.angle * 3.14159265358979f / 180.0f;
        const auto& p = decoration.position;
        const auto place = ts2::scene::Transform::at(p[0], p[1], p[2], turn);
        for (const auto& slot : known->second) {
            const auto* file = effects.with(slot.effect);
            if (file == nullptr) {
                ts2::diag::warnOnce("neighborhood: no effects file has " + slot.effect);
            } else {
                float at[3] = {slot.offset[0], slot.offset[1], slot.offset[2]};
                place.apply(at, true);
                ts2::scene::addGroundDecals(builder, *file, slot.effect, {at[0], at[1], at[2]}, turn, ts2::scene::kNoSnowState, random);
            }
        }
    }
}

} // namespace

ts2::scene::NeighborhoodSceneStats buildNeighborhood(ts2::scene::SceneBuilder& builder, const std::filesystem::path& install,
                                                     const std::filesystem::path& folder, ts2::content::EffectsFiles& effects,
                                                     std::optional<std::uint32_t> variantSeed) {
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
            if (std::isfinite(len) && len > 0) {
                light.sun = {float(d[0] / len), float(d[1] / len), float(d[2] / len)};
            }
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
                                            ts2::content::neighborhoodObjectTypes(products), variantSeed);
    addDecorationEffects(builder, effects, stats, variantSeed);
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
    orbit.useRigEasing();
    orbit.goal = orbit.target;
    orbit.distanceGoal = orbit.distance;
    if (const auto margins = orbit.rig->constrain) {
        // Tiles are 10 units, so a 128-tile neighborhood is 1280 across.
        const float width = static_cast<float>(stats.width) * 10, height = static_cast<float>(stats.height) * 10;
        orbit.limitGoal = [=](Vec3& goal) {
            goal[0] = std::clamp(goal[0], (*margins)[0] * width, std::max((*margins)[0] * width, (1 - (*margins)[2]) * width));
            goal[1] = std::clamp(goal[1], (*margins)[1] * height, std::max((*margins)[1] * height, (1 - (*margins)[3]) * height));
        };
        orbit.limitGoal(*orbit.goal);
        orbit.target = *orbit.goal;
    }
}

} // namespace app
