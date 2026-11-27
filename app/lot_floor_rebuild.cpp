#include "lot_floor_rebuild.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

namespace app {

namespace {

using namespace ts2::scene;

// The lot's own floor parts, by name. The neighbors' houses have instances of the same names.
bool isFloorPart(const Instance& i) {
    constexpr std::array<std::string_view, 5> kFloors{"floors", "stage floors", "ceilings", "floor edges", "wall edge bands"};
    constexpr std::array<std::string_view, 2> kGrids{"storey grid", "active storey grid"};
    if (i.neighborHouse || i.imposterLot) {
        return false;
    }
    if (i.part == Part::UpperGrid || i.part == Part::ActiveGrid) {
        return std::find(kGrids.begin(), kGrids.end(), i.name) != kGrids.end();
    }
    return i.part == Part::Floor && std::find(kFloors.begin(), kFloors.end(), i.name) != kFloors.end();
}

} // namespace

WallUploads putRebuiltFloors(Scene& lot, Scene& rebuilt, const std::vector<std::string>& imageKeys, std::map<std::string, int>& sharedImages,
                             const std::set<std::int32_t>& levels) {
    WallUploads up;
    const auto materialAt = takeMaterials(lot, rebuilt, imageKeys, sharedImages);
    const Transform place = lot.lotWalls ? lot.lotWalls->place : Transform{};
    std::vector<std::size_t> old;
    for (std::size_t i = 0; i < lot.instances.size(); ++i) {
        if (isFloorPart(lot.instances[i]) && (levels.empty() || levels.contains(lot.instances[i].level))) {
            old.push_back(i);
        }
    }
    std::vector<bool> taken(lot.instances.size(), false);
    for (Instance& made : rebuilt.instances) {
        // The rebuild also makes fence rails, which stay as they were.
        if (!isFloorPart(made) || made.mesh < 0 || static_cast<std::size_t>(made.mesh) >= rebuilt.meshes.size()) {
            continue;
        }
        Mesh mesh = std::move(rebuilt.meshes[static_cast<std::size_t>(made.mesh)]);
        for (auto& p : mesh.primitives) {
            p.material = p.material >= 0 && static_cast<std::size_t>(p.material) < materialAt.size() ? materialAt[static_cast<std::size_t>(p.material)] : -1;
        }
        made.transform = place * made.transform;
        const auto same = std::find_if(old.begin(), old.end(), [&](std::size_t i) {
            const Instance& o = lot.instances[i];
            return !taken[i] && o.name == made.name && o.level == made.level;
        });
        if (same != old.end() && lot.instances[*same].mesh >= 0 && static_cast<std::size_t>(lot.instances[*same].mesh) < lot.meshes.size()) {
            const std::size_t at = *same;
            taken[at] = true;
            made.mesh = lot.instances[at].mesh;
            lot.meshes[static_cast<std::size_t>(made.mesh)] = std::move(mesh);
            lot.instances[at] = std::move(made);
            up.meshes.push_back(static_cast<std::size_t>(lot.instances[at].mesh));
            up.instances.push_back(at);
            continue;
        }
        made.mesh = static_cast<int>(lot.meshes.size());
        lot.meshes.push_back(std::move(mesh));
        up.meshes.push_back(static_cast<std::size_t>(made.mesh));
        up.instances.push_back(lot.instances.size());
        lot.instances.push_back(std::move(made));
    }
    for (const std::size_t i : old) {
        const int mesh = lot.instances[i].mesh;
        if (!taken[i] && mesh >= 0 && static_cast<std::size_t>(mesh) < lot.meshes.size() && !lot.meshes[static_cast<std::size_t>(mesh)].primitives.empty()) {
            lot.meshes[static_cast<std::size_t>(mesh)].primitives.clear();
            up.meshes.push_back(static_cast<std::size_t>(mesh));
        }
    }
    return up;
}

} // namespace app
