#pragma once

#include "scene/lot_parts.hpp"
#include "scene/scene.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace app {

// What a wall change left for the renderer: meshes drawn again in place, instances changed or added,
// and images changed. New materials, images and meshes are at the scene's end.
struct WallUploads {
    std::vector<std::size_t> meshes;
    std::vector<std::size_t> instances;
    std::vector<int> images;
};

// True while an instance's object stands, false while it is put aside, none once it is gone for good.
using InstanceStands = std::function<std::optional<bool>(std::size_t instance)>;
// Reuses the lot's materials by name and adds the others, sharing images by key through `sharedImages`.
// Returns each rebuilt material's index in the lot's scene.
std::vector<int> takeMaterials(ts2::scene::Scene& lot, const ts2::scene::Scene& rebuilt, const std::vector<std::string>& imageKeys,
                               std::map<std::string, int>& sharedImages);
// Puts walls drawn again into the lot's scene in place of the old ones, moving `rebuilt`'s meshes and
// instances out. Doors and windows stay tied to the wall standing where theirs stood. A wall taken out
// with objects tied to it is remembered (LotWalls::gone) until one stands there again.
WallUploads putRebuiltWalls(ts2::scene::Scene& lot, ts2::scene::Scene& rebuilt, const std::vector<std::string>& imageKeys,
                            std::map<std::string, int>& sharedImages, const InstanceStands& stands);
// The walls remembered as gone with a tied object standing, the only ones a redraw gives cuts back to.
std::vector<ts2::scene::LotWalls::Gone> goneWithObjectsStanding(const ts2::scene::LotWalls& walls, const InstanceStands& stands);
// A wall whose door or window no longer stands loses its cut. While the object is only put aside, the
// wall is remembered with its cut to get it back later.
void forgetCutsOfObjectsAway(ts2::scene::Scene& lot, const InstanceStands& stands);

} // namespace app
