#pragma once

#include "content/lighting.hpp"
#include "scene/assets.hpp"
#include "scene/scene.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ts2::content {
struct Lot;
}

namespace app {

// What lighting a lot needs that does not change with the lighting state, kept from when the lot was
// first lit so that it can be lit again in another state while it is played.
struct LotLightBase;
// A lot's light in one lighting state, gathered from the game's files and then worked out.
struct LotLight;

// The lighting state the game lights a lot with at the hour its clock was saved at.
std::string savedLightingState(const ts2::content::Lighting& lighting, const ts2::content::Lot& saved);

// Works out a lot's light the way the game does in lighting state `state` (lamps off until the
// lot's own scripts switch them) and fills the scene's floor and wall light maps and its objects'
// lights with it. `saved` is the lot as loaded from `lotPackage`. Returns what lighting the lot again
// needs, or null when the lot gives nothing to light.
std::shared_ptr<LotLightBase> lightLot(ts2::scene::Assets& assets, ts2::scene::Scene& scene, const ts2::content::Lot& saved,
                                       const std::filesystem::path& lotPackage, const std::string& state);

// Lighting the lot again in another state, in three steps. First the lot's lamps, windows and doors
// in `state`, read from the game's files (on the thread that uses `assets`), or null without that state.
std::shared_ptr<LotLight> gatherLotLight(ts2::scene::Assets& assets, std::shared_ptr<LotLightBase> base, const std::string& state);
// Then the light worked out, on any thread (one at a time for a lot).
void workOutLotLight(LotLight& light);
// Then put into the lot's scene (`place` is where the lot stands in it): its sky, its light maps,
// how things it gains or moves are lit, the sun on everything the lot shaded that scripts don't move
// (plants, fences) and the lights of doors and windows. Scripted objects keep theirs until lit
// again (LotLife::relightAll).
struct PutLight {
    std::vector<int> images;            // for the renderer (Renderer::setImages)
    std::vector<std::size_t> instances; // lit anew (Renderer::setLights)
    bool sky = false;                   // the skybox changed (Renderer::setSkybox)
};
PutLight putLotLight(ts2::scene::Scene& scene, const LotLight& light, const ts2::scene::Transform& place);
// The outdoor lights on objects in that state, placed by `place`.
ts2::scene::ObjectLighting outdoorLotLight(const LotLight& light, const ts2::scene::Transform& place);
const std::string& lotLightState(const LotLight& light);
// The state's environment colour (see content::Lighting::State::environment).
const std::array<float, 3>& lotLightEnvironment(const LotLight& light);

} // namespace app
