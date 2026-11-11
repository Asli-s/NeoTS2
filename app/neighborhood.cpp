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
#include "formats/neighborhood_id.hpp"
#include "formats/nhood_paint_rules.hpp"
#include "formats/propset.hpp"
#include "orbit.hpp"
#include "scene/lot_scene.hpp"
#include "scene/lot_skirt.hpp"
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
// The effect the game plays when a neighborhood opens: its sky and weather.
constexpr std::string_view kSkyEffect = "neighborhoodfxdaynight";
constexpr int kMostSkyNesting = 8;
constexpr std::uint8_t kSkyboxMode = 5;
constexpr std::uint32_t kFirstGameFamily = 0x7FE0;

// Whether the neighborhood was saved at night (its Neighborhood ID). Day when it can't be read.
bool savedAtNight(const std::filesystem::path& folder) {
    try {
        ts2::dbpf::Package package{ts2::content::neighborhoodPackage(folder)};
        for (const auto& e : package.entries()) {
            if (e.key.type == ts2::nhoodid::kType) {
                return ts2::nhoodid::parse(package.read(e)).night != 0;
            }
        }
    } catch (const std::exception& ex) {
        ts2::diag::warnOnce(std::string("neighborhood: its identity can't be read, it shows by day: ") + ex.what());
    }
    return false;
}

// The look a lighting state gives the neighborhood, none when the state or its light map image is
// missing. `base` holds the terrain settings that don't depend on the state.
std::optional<NeighborhoodTime> neighborhoodTime(const ts2::content::Lighting& lighting, const std::vector<ts2::content::Product>& products,
                                                 const std::string& name, const ts2::scene::NeighborhoodLight& base) {
    const auto state = lighting.states.find(name);
    if (state == lighting.states.end() || !state->second.lightMap) {
        return std::nullopt;
    }
    NeighborhoodTime time;
    time.terrain = base;
    if (const auto sun = state->second.lights.find("sun"); sun != state->second.lights.end()) {
        const auto& d = sun->second.direction;
        const double len = std::sqrt(double(d[0]) * d[0] + double(d[1]) * d[1] + double(d[2]) * d[2]);
        if (std::isfinite(len) && len > 0) {
            time.terrain.sun = {float(d[0] / len), float(d[1] / len), float(d[2] / len)};
        }
    }
    time.terrain.tint = state->second.lightMapTint;
    auto image = ts2::content::lightingImage(products, *state->second.lightMap);
    if (!image) {
        return std::nullopt;
    }
    time.terrain.lookup = {image->width, image->height, std::move(image->rgba)};
    time.imposters = state->second.environment;
    // The game lights the neighborhood's objects with the state's "NoPortals" copy where there is
    // one, as outdoor objects on a lot are lit.
    const auto* objects = ts2::content::findState(lighting, name + "NoPortals");
    if (objects == nullptr) {
        objects = ts2::content::findState(lighting, name);
    }
    if (objects != nullptr) {
        time.objects = ts2::scene::outdoorLighting(*objects);
        // Unlit materials, such as the trees', take the named state's environment.
        // That is white by day and a dim blue at night.
        time.objects->textureLight = state->second.environment;
    }
    return time;
}

// The texture of the first skybox screen layer `name` plays in `state`, or empty.
std::string skyboxTexture(const ts2::effects::File& file, std::string_view name, std::uint32_t state, int nesting) {
    const ts2::effects::VisualEffect* effect = nesting < kMostSkyNesting ? file.find(std::string(name)) : nullptr;
    if (effect == nullptr) {
        return {};
    }
    for (const auto& c : effect->components) {
        if ((state & c.stateMask) != c.state) {
            continue;
        }
        if (c.type == 0) {
            if (auto texture = skyboxTexture(file, c.name, state, nesting + 1); !texture.empty()) {
                return texture;
            }
        } else if (c.type == static_cast<std::uint8_t>(ts2::effects::BlockType::Screen) && c.index >= 0 &&
                   static_cast<std::size_t>(c.index) < file.screens.size()) {
            const auto& screen = file.screens[static_cast<std::size_t>(c.index)];
            if (screen.mode == kSkyboxMode && !screen.texture.empty()) {
                return screen.texture;
            }
        }
    }
    return {};
}

// The sky cube's faces the neighborhood's sky effect shows by day or at night, or none.
std::vector<ts2::scene::Image> neighborhoodSky(ts2::scene::SceneBuilder& builder, ts2::content::EffectsFiles& effects, bool night) {
    const ts2::effects::File* file = effects.with(kSkyEffect);
    if (file == nullptr) {
        return {};
    }
    const std::string texture = skyboxTexture(*file, kSkyEffect, neighborhoodEffectState(night), 0);
    return texture.empty() ? std::vector<ts2::scene::Image>{} : builder.assets().textureFaces(texture);
}

} // namespace

std::uint32_t neighborhoodEffectState(bool night) {
    constexpr std::uint32_t kAlways = 0x800 | 0x2000;
    return kAlways | (night ? 0u : 0x1u);
}

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
    if (plumbobs.add(builder, *file, kPlumbob, anchors) == 0) {
        ts2::diag::warnOnce("neighborhood: the plumbob effect plays nothing that can be shown yet");
    }
}

namespace {

// The effects of the decorations from the catalog's effects section, as each one's model names
// them at its slots, the game's state without snow: their ground decals laid here, and each slot's
// effect kept in `stats` to play the rest.
void addDecorationEffects(ts2::scene::SceneBuilder& builder, ts2::content::EffectsFiles& effects,
                          ts2::scene::NeighborhoodSceneStats& stats, std::optional<std::uint32_t> seed) {
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
                stats.slotEffects.push_back({slot.effect, {at[0], at[1], at[2]}, turn});
                stats.effectModels.reserve(builder, *file, slot.effect);
            }
        }
    }
}

} // namespace

ts2::scene::NeighborhoodSceneStats buildNeighborhood(ts2::scene::SceneBuilder& builder, const std::filesystem::path& install,
                                                     const std::filesystem::path& folder, ts2::content::EffectsFiles& effects,
                                                     std::optional<std::uint32_t> variantSeed, NeighborhoodTimes* times,
                                                     const std::filesystem::path& highImposters) {
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
    auto day = neighborhoodTime(lighting, products, "neighbourhood", light);
    if (!day || day->terrain.lookup.width == 0) {
        throw std::runtime_error("the neighborhood light map image was not found");
    }
    NeighborhoodTimes looks{std::move(*day), neighborhoodTime(lighting, products, "neighborhoodnight", light), savedAtNight(folder)};
    if (looks.atNight && !looks.night) {
        ts2::diag::warnOnce("neighborhood: the lighting has no usable neighborhoodnight state, it shows by day");
    }
    light = looks.now().terrain;
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
                                            ts2::content::neighborhoodObjectTypes(products), variantSeed, highImposters);
    addDecorationEffects(builder, effects, stats, variantSeed);
    looks.day.sky = neighborhoodSky(builder, effects, false);
    if (looks.night) {
        looks.night->sky = neighborhoodSky(builder, effects, true);
    }
    if (auto& terrain = builder.scene().terrain; terrain && looks.now().sky.size() == 6) {
        terrain->sky = looks.now().sky;
    }
    builder.scene().objectLighting = looks.now().objects;
    // A pool-style sea (not in the original game) takes the water sheet's place. The underwater
    // gradient still darkens what is under it, as the game's does.
    if (auto& terrain = builder.scene().terrain; terrain && builder.poolStyleSea() && ts2::scene::addNeighborhoodSea(builder, *terrain, looks.atNight)) {
        terrain->poolStyleSea = true;
        terrain->seaLight = seaLight(looks);
        // With pool-style ponds too, the lots' ponds are drawn live as the sea is, over their imposters.
        if (builder.poolStylePonds()) {
            ts2::scene::addNeighborhoodPonds(builder, *terrain, stats.lotFootprints, folder, looks.atNight, !highImposters.empty());
        }
    }
    if (times != nullptr) {
        *times = std::move(looks);
    }
    return stats;
}

float seaLight(const NeighborhoodTimes& times) {
    const auto day = ts2::scene::flatGroundLight(times.day.terrain);
    const auto now = ts2::scene::flatGroundLight(times.now().terrain);
    auto brightness = [](const std::array<float, 3>& c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; };
    return brightness(day) > 0 ? std::clamp(brightness(now) / brightness(day), 0.0f, 1.0f) : 1.0f;
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
    orbit.levelKeys = true;
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
