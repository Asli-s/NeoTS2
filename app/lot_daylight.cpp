#include "lot_daylight.hpp"

#include "content/lighting_state.hpp"
#include "diag/log.hpp"
#include "lot_life.hpp"
#include "sim/clock.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <format>

namespace app {

namespace {

// How long the game took to fade its outdoor lights into a new state: 2.5 s into night and 3.2 s
// back into day on one lot, at any game speed. How it comes from the states' transitionTimes is not
// known yet.
constexpr double kFadeSeconds = 2.8;

// Both states' lights at once, each scaled by its share: the same light on things as fading every
// light's colour when they point the same way.
ts2::scene::ObjectLighting between(const ts2::scene::ObjectLighting& from, const ts2::scene::ObjectLighting& to, float t) {
    ts2::scene::ObjectLighting out;
    for (std::size_t i = 0; i < 3; ++i) {
        out.ambient[i] = from.ambient[i] + (to.ambient[i] - from.ambient[i]) * t;
    }
    for (const auto& [lights, share] : {std::pair{&from.lights, 1 - t}, std::pair{&to.lights, t}}) {
        for (auto light : *lights) {
            for (auto& c : light.color) {
                c *= share;
            }
            out.lights.push_back(light);
        }
    }
    return out;
}

} // namespace

LotDaylight::LotDaylight(std::shared_ptr<LotLightBase> base, std::string state, const std::array<float, 3>& environment,
                         const ts2::scene::Transform& place)
    : base_(std::move(base)), state_(std::move(state)), place_(place), environmentFrom_(environment), environment_(environment) {}

LotDaylight::~LotDaylight() {
    if (working_.valid()) {
        working_.wait();
    }
}

void LotDaylight::update(double seconds, std::span<const std::int16_t> globals, ts2::scene::Assets& assets, ts2::scene::Scene& scene,
                         ts2::render::IRenderer& renderer, LotLife& life) {
    if (!base_) {
        return;
    }
    if (fade_ < 1) {
        fade_ = std::min(1.0, fade_ + seconds / kFadeSeconds);
        scene.objectLighting = fade_ < 1 ? between(from_, to_, static_cast<float>(fade_)) : to_;
        renderer.setObjectLighting(scene.objectLighting);
        renderer.setLightMapFade(static_cast<float>(fade_));
        std::array<float, 3> light;
        for (std::size_t i = 0; i < 3; ++i) {
            light[i] = environmentFrom_[i] + (environment_[i] - environmentFrom_[i]) * static_cast<float>(fade_);
        }
        renderer.setImposterLight(light);
    }
    if (working_.valid()) {
        if (working_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return;
        }
        try {
            if (auto light = working_.get()) {
                put(*light, scene, renderer, life);
            }
        } catch (const std::exception& ex) {
            ts2::diag::error(std::format("lot light: lighting the lot again failed: {}", ex.what()));
        }
    }
    if (globals.size() <= ts2::sim::global::kTimeOfDay || globals[ts2::sim::global::kTimeOfDay] == timeOfDay_) {
        return;
    }
    timeOfDay_ = globals[ts2::sim::global::kTimeOfDay];
    const auto wanted = ts2::content::lotLightingState(assets.lighting(), timeOfDay_);
    if (wanted.empty() || wanted == state_) {
        return;
    }
    // The lot's lamps and windows are read here, where the game's files are. The rest runs beside.
    std::shared_ptr<LotLight> light;
    try {
        light = gatherLotLight(assets, base_, wanted);
    } catch (const std::exception& ex) {
        ts2::diag::error(std::format("lot light: reading the lot's lights for {} failed: {}", wanted, ex.what()));
    }
    if (!light) {
        state_ = wanted;  // not tried again until the state changes
        return;
    }
    state_ = lotLightState(*light);
    working_ = std::async(std::launch::async, [light = std::move(light)] {
        workOutLotLight(*light);
        return light;
    });
}

void LotDaylight::settle(ts2::scene::Scene& scene, ts2::render::IRenderer& renderer, LotLife& life) {
    if (working_.valid()) {
        working_.wait();
        try {
            if (auto light = working_.get()) {
                put(*light, scene, renderer, life);
            }
        } catch (const std::exception& ex) {
            ts2::diag::error(std::format("lot light: lighting the lot again failed: {}", ex.what()));
        }
    }
    if (fade_ < 1) {
        fade_ = 1;
        scene.objectLighting = to_;
        renderer.setObjectLighting(scene.objectLighting);
        renderer.setLightMapFade(1);
        renderer.setImposterLight(environment_);
    }
}

void LotDaylight::put(LotLight& light, ts2::scene::Scene& scene, ts2::render::IRenderer& renderer, LotLife& life) {
    const auto put = putLotLight(scene, light, place_);
    renderer.setImages(scene, put.images, true);
    renderer.setLightMapFade(0);
    renderer.setNight(ts2::content::stateTime(lotLightState(light)) == ts2::content::StateTime::Night);
    if (put.sky) {
        renderer.setSkybox(scene.skybox);
    }
    for (const std::size_t instance : put.instances) {
        renderer.setLights(scene, instance);
    }
    life.relightAll(scene);
    // A fade still running ends where it was going, so the lights don't add up.
    from_ = fade_ < 1 ? to_ : scene.objectLighting.value_or(ts2::scene::ObjectLighting{});
    to_ = outdoorLotLight(light, place_);
    environmentFrom_ = environment_;
    environment_ = lotLightEnvironment(light);
    fade_ = 0;
}

} // namespace app
