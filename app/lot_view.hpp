#pragma once

#include "orbit.hpp"
#include "render/renderer.hpp"
#include "scene/lot_skirt.hpp"
#include "scene/neighborhood_scene.hpp"
#include "scene/rig.hpp"
#include "scene/scene.hpp"
#include "scene/view.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace app {

// How far the camera rises per storey.
constexpr float kStorey = 3.0f;

// Points the camera at the middle of everything placed in the scene, far enough to see it all.
inline void frameScene(const ts2::scene::Scene& scene, Orbit& orbit) {
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (const auto& instance : scene.instances) {
        if (instance.mesh < 0 || scene.meshes[static_cast<std::size_t>(instance.mesh)].name == "terrain") {
            continue;
        }
        const auto& t = instance.transform.m;
        const Vec3 at{t[3], t[7], t[11]};
        for (std::size_t k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], at[k]);
            hi[k] = std::max(hi[k], at[k]);
        }
    }
    if (lo[0] > hi[0]) {
        return;
    }
    orbit.target = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    orbit.distance = std::max(10.0f, 1.2f * std::max(hi[0] - lo[0], hi[1] - lo[1]));
}

// Looking at a lot: which storey, how walls show, roofs on or off, and the keys that change them
// (Q/E or comma/period turn a quarter, Page Up/Down change storey, Home/End (or Tab)
// the next or previous wall mode, R roofs; 1/2/3 are the game's speed keys).
struct LotView {
    ts2::scene::View view;
    int ground = 0, top = 0;  // the lowest storey worth showing on its own, and the highest

    // Frames the lot and starts on its top storey with walls up.
    void start(const ts2::scene::Scene& scene, Orbit& orbit) {
        orbit = {};
        frameScene(scene, orbit);
        view = {};
        top = ts2::scene::topLevel(scene);
        ground = ts2::scene::groundLevel(scene);
        view.level = top;
    }

    void changeLevel(int delta, Orbit& orbit) {
        const int level = std::clamp(view.level + delta, ground, top);
        orbit.target[2] += static_cast<float>(level - view.level) * kStorey;
        view.level = level;
    }

    // Returns whether the key was one of the view's.
    bool key(SDL_Keycode key, Orbit& orbit) {
        switch (key) {
        case SDLK_Q:
        case SDLK_COMMA:
            orbit.turn(-1);
            return true;
        case SDLK_E:
        case SDLK_PERIOD:
            orbit.turn(1);
            return true;
        case SDLK_PAGEUP:
            changeLevel(1, orbit);
            return true;
        case SDLK_PAGEDOWN:
            changeLevel(-1, orbit);
            return true;
        case SDLK_HOME:
        case SDLK_TAB:
            view.walls = static_cast<ts2::scene::WallView>((static_cast<int>(view.walls) + 1) % 3);
            return true;
        case SDLK_END:
            view.walls = static_cast<ts2::scene::WallView>((static_cast<int>(view.walls) + 2) % 3);
            return true;
        case SDLK_R:
            view.roofs = !view.roofs;
            return true;
        default:
            return false;
        }
    }
};

// Adds a scene's images, materials, meshes, instances, walls, light maps and object lighting to
// another, its instances moved by `place`.
inline void appendScene(ts2::scene::Scene& out, const ts2::scene::Scene& add, const ts2::scene::Transform& place) {
    using namespace ts2::scene;
    const int images = static_cast<int>(out.images.size()), materials = static_cast<int>(out.materials.size());
    const int meshes = static_cast<int>(out.meshes.size()), walls = static_cast<int>(out.walls.size());
    const std::size_t instances = out.instances.size();
    out.images.insert(out.images.end(), add.images.begin(), add.images.end());
    for (Material m : add.materials) {
        if (m.image >= 0) {
            m.image += images;
        }
        out.materials.push_back(std::move(m));
    }
    for (Mesh m : add.meshes) {
        for (auto& p : m.primitives) {
            if (p.material >= 0) {
                p.material += materials;
            }
        }
        out.meshes.push_back(std::move(m));
    }
    for (Instance i : add.instances) {
        if (i.mesh >= 0) {
            i.mesh += meshes;
        }
        if (i.lightMap >= 0) {
            i.lightMap += images;
        }
        i.transform = place * i.transform;
        for (auto& w : i.walls) {
            w += walls;
        }
        out.instances.push_back(std::move(i));
    }
    for (Actor a : add.actors) {
        a.place = place * a.place;
        for (auto& s : a.skinned) {
            s.instance += instances;
        }
        for (auto& h : a.held) {
            h.instance += instances;
        }
        out.actors.push_back(std::move(a));
    }
    if (add.lightMaps && !out.lightMaps) {
        auto& maps = out.lightMaps.emplace(*add.lightMaps);
        for (auto& f : maps.floors) {
            if (f >= 0) {
                f += images;
            }
        }
        if (maps.walls >= 0) {
            maps.walls += images;
        }
    }
    // The lights turn with the scene, as its objects' normals do.
    if (add.objectLighting && !out.objectLighting) {
        auto& lighting = out.objectLighting.emplace(*add.objectLighting);
        for (auto& l : lighting.lights) {
            place.apply(l.direction.data(), false);
            const float len = std::sqrt(l.direction[0] * l.direction[0] + l.direction[1] * l.direction[1] + l.direction[2] * l.direction[2]);
            if (len > 0) {
                for (auto& c : l.direction) {
                    c /= len;
                }
            }
        }
    }
    for (WallInfo w : add.walls) {
        float a[3] = {w.x0, w.y0, 0}, b[3] = {w.x1, w.y1, 0};
        place.apply(a, true);
        place.apply(b, true);
        w.x0 = a[0];
        w.y0 = a[1];
        w.x1 = b[0];
        w.y1 = b[1];
        out.walls.push_back(w);
    }
}

// A lot shown as the game's lot view shows it: the lot itself where its imposter stood, and around
// it, within the skirt's area, the ground around a lot (`skirt`, in world coordinates), the other
// lots' imposters and the neighborhood's plants. The neighborhood's own far-view ground, water and
// roads are not drawn.
inline ts2::scene::Scene lotInNeighborhood(const ts2::scene::Scene& hood, const ts2::scene::Scene& lot,
                                           const ts2::scene::Scene& skirt, const ts2::scene::LotFootprint& where,
                                           const ts2::scene::SkirtArea& area) {
    using namespace ts2::scene;
    Scene out;
    out.terrain = hood.terrain;
    out.skybox = lot.skybox;
    out.clip = skirt.clip;
    const float tile = out.terrain ? out.terrain->tileSize : 10.0f;
    const float ax0 = static_cast<float>(area.x0) * tile, ay0 = static_cast<float>(area.y0) * tile;
    const float ax1 = static_cast<float>(area.x1) * tile, ay1 = static_cast<float>(area.y1) * tile;
    // The neighborhood's plants within the area.
    Scene plants = hood;
    plants.terrain.reset();
    std::erase_if(plants.instances, [&](const Instance& i) {
        const float x = i.transform.m[3], y = i.transform.m[7];
        return x < ax0 || x > ax1 || y < ay0 || y > ay1;
    });
    appendScene(out, plants, Transform{});
    appendScene(out, skirt, Transform{});
    appendScene(out, lot, where.place);
    if (out.terrain) {
        auto& t = *out.terrain;
        // Other lots' imposters within the area; not this lot's. In lot view the game stands them on
        // their lots, without the neighborhood view's lift.
        std::erase_if(t.imposters, [&](const PaintedTerrain::ImposterPart& p) {
            if (p.lot == where.id || p.vertices.size() < 3) {
                return true;
            }
            const float x = p.vertices[0], y = p.vertices[1];
            return x < ax0 || x > ax1 || y < ay0 || y > ay1;
        });
        for (auto& p : t.imposters) {
            for (std::size_t i = 2; i < p.vertices.size(); i += 7) {
                p.vertices[i] -= t.imposterLift;
            }
        }
        t.imposterLift = 0;
        t.hiddenTiles.assign(static_cast<std::size_t>(t.columns - 1) * (t.rows - 1), 1);
        t.roads.clear();
        t.roadTextures.clear();
    }
    return out;
}

// Poses a scene's actors (its Sims) `seconds` after they started and hands their joints and the
// places of what they hold to the renderer.
inline void animateActors(const ts2::scene::Scene& scene, double seconds, ts2::render::Renderer& renderer,
                          ts2::scene::ScenePose& pose) {
    ts2::scene::poseActors(scene, seconds, pose);
    for (const auto& [instance, joints] : pose.joints) {
        renderer.setJoints(instance, joints);
    }
    for (const auto& [instance, place] : pose.places) {
        renderer.setPlace(instance, place);
    }
}

// Moves a camera set up in a lot's own coordinates to where the lot is in its neighborhood.
inline void placeCamera(Orbit& orbit, const ts2::scene::LotFootprint& where) {
    float t[3] = {orbit.target[0], orbit.target[1], orbit.target[2]};
    where.place.apply(t, true);
    orbit.target = {t[0], t[1], t[2]};
    const float turn = std::atan2(where.place.m[4], where.place.m[0]);
    orbit.yaw += turn;
    orbit.yawGoal += turn;
}

} // namespace app
