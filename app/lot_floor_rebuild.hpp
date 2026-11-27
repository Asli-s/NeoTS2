#pragma once

#include "lot_wall_rebuild.hpp"
#include "scene/scene.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace app {

// Puts floors drawn again (see ts2::scene::rebuildLotFloors) into the lot's scene. Each part takes over
// the mesh of the one it replaces, and parts no longer drawn draw nothing. With `levels`, only those
// grid levels were drawn again. Returns what the renderer must take.
WallUploads putRebuiltFloors(ts2::scene::Scene& lot, ts2::scene::Scene& rebuilt, const std::vector<std::string>& imageKeys,
                             std::map<std::string, int>& sharedImages, const std::set<std::int32_t>& levels = {});

} // namespace app
