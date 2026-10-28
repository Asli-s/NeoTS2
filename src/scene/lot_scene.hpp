#pragma once

#include "content/lighting.hpp"
#include "scene/builder.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ts2::content {
struct Lot;
}

namespace ts2::scene {

// What adding a lot to a scene found.
struct LotSceneStats {
    std::size_t objects = 0;           // objects placed
    std::size_t sims = 0;              // Sims placed
    std::size_t inSlots = 0;           // of those, held in another object's slot
    std::size_t withoutRecord = 0;     // objects with no scene record
    std::size_t withoutModel = 0;      // objects whose model wasn't found
    std::size_t walls = 0, coveredWalls = 0, wallCuts = 0;
    std::size_t fenceRails = 0, fencePosts = 0;
    std::size_t floorPatterns = 0, roofs = 0;
    std::size_t terrainPaints = 0;  // paints drawn over the terrain's canvas
    // One line per object (where it is, what it shows), when asked for.
    std::vector<std::string> objectLog;
};

// Adds a saved lot (Neighborhoods/<hood>/Lots/<hood>_Lot<n>.package) to the builder's scene: its
// objects in place (including those held in other objects' slots), walls with their coverings and
// door and window openings, floors and floor slabs, fences, roofs and the terrain.
LotSceneStats addLot(SceneBuilder& builder, const std::filesystem::path& lot, bool logObjects = false);
// The same with the lot already loaded from that package (content::loadLot).
LotSceneStats addLot(SceneBuilder& builder, const content::Lot& loaded, const std::filesystem::path& lot, bool logObjects = false);

// How the game lights objects outdoors in a lighting state: the skylight as the ambient, then the
// sun, fill and sky lights, each as strength * (colour - skylight) per channel. Sims also get the
// state's simFill1 and simFill2 the same way (ObjectLighting::simLights).
[[nodiscard]] ObjectLighting outdoorLighting(const content::Lighting::State& state);
// The same for the "day" state, none without one.
[[nodiscard]] std::optional<ObjectLighting> objectDaylight(const content::Lighting& lighting);

} // namespace ts2::scene
