#include "lot_wall_rebuild.hpp"

#include "scene/lot_parts.hpp"

#include <cmath>
#include <tuple>
#include <unordered_map>

namespace app {

namespace {

using namespace ts2::scene;

// A wall's middle, doubled so it is whole, and its level.
std::tuple<long, long, int> placeOf(const ThickWall& w) {
    return {std::lround(w.q.x0 + w.q.x1), std::lround(w.q.y0 + w.q.y1), w.level};
}

void growImage(Image& image, std::uint32_t height, const std::array<std::uint8_t, 4>& fill) {
    if (height <= image.height) {
        return;
    }
    const std::size_t old = image.rgba.size();
    image.rgba.resize(static_cast<std::size_t>(image.width) * height * 4);
    for (std::size_t i = old; i + 3 < image.rgba.size(); i += 4) {
        std::copy(fill.begin(), fill.end(), image.rgba.begin() + static_cast<std::ptrdiff_t>(i));
    }
    image.height = height;
}

// Whether `made`, its materials mapped through `materialAt`, draws just what `had` draws.
bool sameDrawing(const Mesh& had, const Mesh& made, const std::vector<int>& materialAt) {
    if (had.primitives.size() != made.primitives.size()) {
        return false;
    }
    for (std::size_t k = 0; k < had.primitives.size(); ++k) {
        const auto& a = had.primitives[k];
        const auto& b = made.primitives[k];
        const int material = b.material >= 0 && static_cast<std::size_t>(b.material) < materialAt.size() ? materialAt[static_cast<std::size_t>(b.material)] : -1;
        if (a.material != material || a.group != b.group || a.alphaSort != b.alphaSort || a.positions != b.positions ||
            a.normals != b.normals || a.uvs != b.uvs || a.alphas != b.alphas || a.tangents != b.tangents || a.colors != b.colors ||
            a.lightUvs != b.lightUvs || a.indices != b.indices || !a.joints.empty() || !b.joints.empty()) {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<int> takeMaterials(Scene& lot, const Scene& rebuilt, const std::vector<std::string>& imageKeys, std::map<std::string, int>& sharedImages) {
    const auto image = [&](int i) -> int {
        if (i < 0 || static_cast<std::size_t>(i) >= rebuilt.images.size()) {
            return -1;
        }
        const std::string key = static_cast<std::size_t>(i) < imageKeys.size() ? imageKeys[static_cast<std::size_t>(i)] : std::string();
        if (const auto shared = key.empty() ? sharedImages.end() : sharedImages.find(key); shared != sharedImages.end()) {
            return shared->second;
        }
        const int at = static_cast<int>(lot.images.size());
        lot.images.push_back(rebuilt.images[static_cast<std::size_t>(i)]);
        if (!key.empty()) {
            sharedImages.emplace(key, at);
        }
        return at;
    };

    std::unordered_map<std::string, int> byName;
    for (std::size_t i = 0; i < lot.materials.size(); ++i) {
        byName.try_emplace(lot.materials[i].name, static_cast<int>(i));
    }
    std::vector<int> materialAt(rebuilt.materials.size(), -1);
    for (std::size_t i = 0; i < rebuilt.materials.size(); ++i) {
        Material m = rebuilt.materials[i];
        if (const auto known = byName.find(m.name); known != byName.end()) {
            materialAt[i] = known->second;
            continue;
        }
        m.image = image(m.image);
        m.normalMap = image(m.normalMap);
        m.causticsImage = image(m.causticsImage);
        m.mask = image(m.mask);
        m.detail = image(m.detail);
        for (int& ripple : m.ripples) {
            ripple = image(ripple);
        }
        m.envCube = -1;  // walls and floors have no environment cube
        materialAt[i] = static_cast<int>(lot.materials.size());
        byName.emplace(m.name, materialAt[i]);
        lot.materials.push_back(std::move(m));
    }
    return materialAt;
}

WallUploads putRebuiltWalls(Scene& lot, Scene& rebuilt, const std::vector<std::string>& imageKeys, std::map<std::string, int>& sharedImages,
                            const InstanceStands& stands) {
    WallUploads up;
    if (!lot.lotWalls || !rebuilt.lotWalls) {
        return up;
    }
    const LotWalls& before = *lot.lotWalls;
    const Transform& place = before.place;
    const auto materialAt = takeMaterials(lot, rebuilt, imageKeys, sharedImages);

    if (lot.lightMaps && rebuilt.lightMaps) {
        auto& maps = *lot.lightMaps;
        maps.wallRects = rebuilt.lightMaps->wallRects;
        maps.outdoorFaces = rebuilt.lightMaps->outdoorFaces;
        int bottom = 0;
        for (const auto& [key, r] : maps.wallRects) {
            bottom = std::max(bottom, r.y1);
        }
        if (maps.walls >= 0 && static_cast<std::size_t>(maps.walls) < lot.images.size() &&
            static_cast<std::uint32_t>(bottom) > lot.images[static_cast<std::size_t>(maps.walls)].height) {
            growImage(lot.images[static_cast<std::size_t>(maps.walls)], static_cast<std::uint32_t>(bottom), {128, 128, 128, 128});
            up.images.push_back(maps.walls);
            if (maps.wallIncidence >= 0 && static_cast<std::size_t>(maps.wallIncidence) < lot.images.size()) {
                growImage(lot.images[static_cast<std::size_t>(maps.wallIncidence)], std::max(1u, static_cast<std::uint32_t>(bottom) / 2), {128, 128, 255, 255});
                up.images.push_back(maps.wallIncidence);
            }
        }
    }

    const int first = std::clamp(before.firstWall, 0, static_cast<int>(lot.walls.size()));
    const auto& newWalls = rebuilt.lotWalls->walls;
    std::vector<WallInfo> infos;
    for (const WallInfo& w : rebuilt.walls) {
        WallInfo placed = w;
        float a[3] = {w.x0, w.y0, 0}, b[3] = {w.x1, w.y1, 0};
        place.apply(a, true);
        place.apply(b, true);
        placed.x0 = a[0];
        placed.y0 = a[1];
        placed.x1 = b[0];
        placed.y1 = b[1];
        infos.push_back(placed);
    }
    const auto oldCount = static_cast<std::ptrdiff_t>(std::min(before.walls.size(), lot.walls.size() - static_cast<std::size_t>(first)));
    lot.walls.erase(lot.walls.begin() + first, lot.walls.begin() + first + oldCount);
    lot.walls.insert(lot.walls.begin() + first, infos.begin(), infos.end());

    // Objects tied to a wall stay tied to the wall standing where it stood.
    std::map<std::tuple<long, long, int>, int> newAt;
    for (std::size_t i = 0; i < newWalls.size(); ++i) {
        newAt.emplace(placeOf(newWalls[i]), first + static_cast<int>(i));
    }
    std::vector<bool> isWallPart(lot.instances.size(), false);
    for (const std::size_t i : before.instances) {
        if (i < isWallPart.size()) {
            isWallPart[i] = true;
        }
    }
    std::map<std::tuple<long, long, int>, LotWalls::Gone> gone;
    for (const auto& g : before.gone) {
        gone.emplace(placeOf(g.wall), g);
    }
    for (std::size_t i = 0; i < lot.instances.size(); ++i) {
        auto& walls = lot.instances[i].walls;
        if (isWallPart[i] || walls.empty()) {
            continue;
        }
        std::vector<int> kept;
        bool changed = false;
        for (const int w : walls) {
            if (w < first || w >= first + oldCount) {
                kept.push_back(w);
                continue;
            }
            const ThickWall& was = before.walls[static_cast<std::size_t>(w - first)];
            const auto there = newAt.find(placeOf(was));
            if (there != newAt.end()) {
                kept.push_back(there->second);
            } else {
                auto& g = gone.try_emplace(placeOf(was), LotWalls::Gone{was, {}}).first->second;
                if (std::find(g.tied.begin(), g.tied.end(), i) == g.tied.end()) {
                    g.tied.push_back(i);
                }
            }
            changed = changed || there == newAt.end() || there->second != w;
        }
        if (changed) {
            walls = std::move(kept);
            up.instances.push_back(i);
        }
    }
    // A wall standing again takes back its tied objects that still stand.
    std::vector<LotWalls::Gone> stillGone;
    for (auto& [at, g] : gone) {
        std::erase_if(g.tied, [&](std::size_t i) { return i >= lot.instances.size() || !stands(i).has_value(); });
        if (g.tied.empty()) {
            continue;
        }
        const auto there = newAt.find(at);
        if (there == newAt.end()) {
            stillGone.push_back(std::move(g));
            continue;
        }
        // Kept while any of its objects is put aside, to give the cut back later.
        if (std::any_of(g.tied.begin(), g.tied.end(), [&](std::size_t i) { return !stands(i).value_or(false); })) {
            stillGone.push_back(g);
        }
        for (const std::size_t i : g.tied) {
            if (!stands(i).value_or(false)) {
                continue;
            }
            auto& walls = lot.instances[i].walls;
            if (std::find(walls.begin(), walls.end(), there->second) == walls.end()) {
                walls.push_back(there->second);
                up.instances.push_back(i);
            }
        }
    }

    // The new parts take over the old parts' instances. Rails of one fence share a mesh, which is never
    // drawn over while another instance uses it.
    auto kept = std::make_shared<LotWalls>(*rebuilt.lotWalls);
    kept->firstWall = first;
    kept->place = place;
    kept->instances.clear();
    std::vector<std::size_t> uses(lot.meshes.size(), 0);
    for (const Instance& i : lot.instances) {
        if (i.mesh >= 0 && static_cast<std::size_t>(i.mesh) < uses.size()) {
            ++uses[static_cast<std::size_t>(i.mesh)];
        }
    }
    std::vector<int> unused;  // meshes no instance uses any more, to draw new ones in
    const auto release = [&](const Instance& i) {
        if (i.mesh >= 0 && static_cast<std::size_t>(i.mesh) < uses.size() && --uses[static_cast<std::size_t>(i.mesh)] == 0) {
            lot.meshes[static_cast<std::size_t>(i.mesh)].primitives.clear();
            up.meshes.push_back(static_cast<std::size_t>(i.mesh));
            unused.push_back(i.mesh);
        }
    };
    std::map<int, int> meshOf;  // a rebuilt mesh's place in the scene, once put there
    const auto put = [&](int made) {
        if (const auto known = meshOf.find(made); known != meshOf.end()) {
            return known->second;
        }
        Mesh mesh = std::move(rebuilt.meshes[static_cast<std::size_t>(made)]);
        for (auto& p : mesh.primitives) {
            p.material = p.material >= 0 && static_cast<std::size_t>(p.material) < materialAt.size() ? materialAt[static_cast<std::size_t>(p.material)] : -1;
        }
        int at = 0;
        if (!unused.empty()) {
            at = unused.back();
            unused.pop_back();
            lot.meshes[static_cast<std::size_t>(at)] = std::move(mesh);
        } else {
            at = static_cast<int>(lot.meshes.size());
            lot.meshes.push_back(std::move(mesh));
            uses.push_back(0);
        }
        up.meshes.push_back(static_cast<std::size_t>(at));
        meshOf.emplace(made, at);
        return at;
    };
    std::size_t slot = 0;
    for (Instance& made : rebuilt.instances) {
        made.transform = place * made.transform;
        for (int& w : made.walls) {
            w += first;
        }
        const int madeMesh = made.mesh;
        if (madeMesh < 0 || static_cast<std::size_t>(madeMesh) >= rebuilt.meshes.size()) {
            continue;
        }
        if (slot < before.instances.size() && before.instances[slot] < lot.instances.size()) {
            const std::size_t at = before.instances[slot++];
            // A part drawn just as before keeps its mesh, which isn't uploaded again.
            const int had = lot.instances[at].mesh;
            const auto known = meshOf.find(madeMesh);
            if (had >= 0 && static_cast<std::size_t>(had) < lot.meshes.size() && madeMesh >= 0 && static_cast<std::size_t>(madeMesh) < rebuilt.meshes.size() &&
                (known != meshOf.end() ? known->second == had
                                       : sameDrawing(lot.meshes[static_cast<std::size_t>(had)], rebuilt.meshes[static_cast<std::size_t>(madeMesh)], materialAt))) {
                meshOf.emplace(madeMesh, had);
                made.mesh = had;
                lot.instances[at] = std::move(made);
                up.instances.push_back(at);
                kept->instances.push_back(at);
                continue;
            }
            release(lot.instances[at]);
            made.mesh = put(madeMesh);
            ++uses[static_cast<std::size_t>(made.mesh)];
            lot.instances[at] = std::move(made);
            up.instances.push_back(at);
            kept->instances.push_back(at);
            continue;
        }
        made.mesh = put(madeMesh);
        ++uses[static_cast<std::size_t>(made.mesh)];
        up.instances.push_back(lot.instances.size());
        kept->instances.push_back(lot.instances.size());
        lot.instances.push_back(std::move(made));
    }
    // A leftover old part draws nothing until a later change needs it again.
    for (; slot < before.instances.size(); ++slot) {
        const std::size_t at = before.instances[slot];
        if (at >= lot.instances.size()) {
            continue;
        }
        Instance& spare = lot.instances[at];
        release(spare);
        spare.mesh = -1;
        spare.walls.clear();
        up.instances.push_back(at);
        kept->instances.push_back(at);
    }
    kept->gone = std::move(stillGone);
    lot.lotWalls = std::move(kept);
    return up;
}

void forgetCutsOfObjectsAway(Scene& lot, const InstanceStands& stands) {
    if (!lot.lotWalls) {
        return;
    }
    auto walls = std::make_shared<LotWalls>(*lot.lotWalls);
    const int first = walls->firstWall;
    std::vector<bool> isWallPart(lot.instances.size(), false);
    for (const std::size_t i : walls->instances) {
        if (i < isWallPart.size()) {
            isWallPart[i] = true;
        }
    }
    bool changed = false;
    for (std::size_t i = 0; i < lot.instances.size(); ++i) {
        if (isWallPart[i] || lot.instances[i].walls.empty()) {
            continue;
        }
        const auto standing = stands(i);
        if (standing.value_or(false)) {
            continue;
        }
        for (const int w : lot.instances[i].walls) {
            if (w < first || static_cast<std::size_t>(w - first) >= walls->walls.size()) {
                continue;
            }
            ThickWall& wall = walls->walls[static_cast<std::size_t>(w - first)];
            if (standing.has_value()) {
                auto g = std::find_if(walls->gone.begin(), walls->gone.end(), [&](const LotWalls::Gone& e) { return placeOf(e.wall) == placeOf(wall); });
                if (g == walls->gone.end()) {
                    walls->gone.push_back({wall, {i}});
                } else if (std::find(g->tied.begin(), g->tied.end(), i) == g->tied.end()) {
                    g->tied.push_back(i);
                }
            }
            changed = changed || !wall.leftMask.empty() || !wall.rightMask.empty();
            wall.leftMask.clear();
            wall.rightMask.clear();
        }
    }
    if (changed) {
        lot.lotWalls = std::move(walls);
    }
}

std::vector<LotWalls::Gone> goneWithObjectsStanding(const LotWalls& walls, const InstanceStands& stands) {
    std::vector<LotWalls::Gone> out;
    for (const auto& g : walls.gone) {
        if (std::any_of(g.tied.begin(), g.tied.end(), [&](std::size_t i) { return stands(i).value_or(false); })) {
            out.push_back(g);
        }
    }
    return out;
}

} // namespace app
