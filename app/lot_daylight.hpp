#pragma once

#include "lot_light.hpp"
#include "render/renderer.hpp"
#include "scene/assets.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <future>
#include <memory>
#include <span>
#include <string>

namespace app {

class LotLife;

// Keeps a played lot lit for its time of day. When the lighting state the game's clock picks
// changes (7 am and 7 pm as shipped), the lot's light is worked out again in the background and put
// in with the state's sky and the ground around the lot's colours, and the outdoor lights on
// objects and the light maps fade to the new ones over a few real seconds, as the game's do. The
// lights of things in rooms change at once (whether the game fades them too is not known yet).
class LotDaylight {
public:
    // `state` is what the lot was lit with (with `environment`, its environment colour), `place`
    // where the lot stands in the scene.
    LotDaylight(std::shared_ptr<LotLightBase> base, std::string state, const std::array<float, 3>& environment,
                const ts2::scene::Transform& place);
    ~LotDaylight();
    LotDaylight(const LotDaylight&) = delete;
    LotDaylight& operator=(const LotDaylight&) = delete;

    // Once a frame, after the simulation's update, with the real seconds since the last frame.
    void update(double seconds, std::span<const std::int16_t> globals, ts2::scene::Assets& assets, ts2::scene::Scene& scene,
                ts2::render::Renderer& renderer, LotLife& life);

    // Waits for light being worked out and puts it in with its fade done (for pictures).
    void settle(ts2::scene::Scene& scene, ts2::render::Renderer& renderer, LotLife& life);

private:
    void put(LotLight& light, ts2::scene::Scene& scene, ts2::render::Renderer& renderer, LotLife& life);

    std::shared_ptr<LotLightBase> base_;
    std::string state_;  // the state the lot is lit with, or being lit with
    ts2::scene::Transform place_;
    int timeOfDay_ = -1;
    std::future<std::shared_ptr<LotLight>> working_;
    // The outdoor lights fading from one state's to the next's.
    ts2::scene::ObjectLighting from_, to_;
    std::array<float, 3> environmentFrom_{1, 1, 1}, environment_{1, 1, 1};  // lot imposters' light
    double fade_ = 1;  // 0 .. 1, 1 when done
};

} // namespace app
