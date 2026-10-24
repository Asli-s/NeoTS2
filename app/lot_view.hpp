#pragma once

#include "diag/log.hpp"
#include "orbit.hpp"
#include "render/renderer.hpp"
#include "scene/lot_skirt.hpp"
#include "scene/neighborhood_plumbobs.hpp"
#include "scene/neighborhood_scene.hpp"
#include "scene/rig.hpp"
#include "scene/scene.hpp"
#include "scene/view.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <string>
#include <utility>

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
// the next or previous wall mode, R roofs). 1/2/3 are the game's speed keys.
struct LotView {
    ts2::scene::View view;
    int ground = 0, top = 0;  // the lowest storey the view goes down to (the ground), and the highest

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
        // A camera with a goal finds its height from the floors of the storey shown and eases there.
        if (!orbit.goal) {
            orbit.target[2] += static_cast<float>(level - view.level) * kStorey;
        }
        if (level != view.level) {
            ts2::diag::step(std::format("view: storey {}", level - ground + 1));
        }
        view.level = level;
    }

    // The game's wall modes: 0 walls down, 1 cutaway, 2 walls up, 3 walls up with the roofs.
    int wallMode() const {
        switch (view.walls) {
        case ts2::scene::WallView::Down: return 0;
        case ts2::scene::WallView::Cutaway: return 1;
        default: return view.roofs ? 3 : 2;
        }
    }
    void setWallMode(int mode) {
        static constexpr ts2::scene::WallView kWalls[] = {ts2::scene::WallView::Down, ts2::scene::WallView::Cutaway,
                                                          ts2::scene::WallView::Up, ts2::scene::WallView::Up};
        static constexpr const char* kNames[] = {"walls down", "walls cutaway", "walls up", "walls up with roofs"};
        mode = std::clamp(mode, 0, 3);
        if (mode != wallMode()) {
            ts2::diag::step(std::string("view: ") + kNames[mode]);
        }
        view.walls = kWalls[mode];
        view.roofs = mode == 3;
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
            setWallMode((wallMode() + 3) % 4);
            return true;
        case SDLK_END:
            setWallMode((wallMode() + 1) % 4);
            return true;
        case SDLK_R:
            view.roofs = !view.roofs;
            ts2::diag::step(view.roofs ? "view: roofs on" : "view: roofs off");
            return true;
        default:
            return false;
        }
    }
};

// Lights moved with a scene by `place`: they turn as its objects' normals do, and point lights move.
inline void placeLights(ts2::scene::ObjectLighting& lighting, const ts2::scene::Transform& place) {
    for (auto& l : lighting.lights) {
        place.apply(l.direction.data(), false);
        const float len = std::sqrt(l.direction[0] * l.direction[0] + l.direction[1] * l.direction[1] + l.direction[2] * l.direction[2]);
        if (len > 0) {
            for (auto& c : l.direction) {
                c /= len;
            }
        }
        if (l.point) {
            place.apply(l.position.data(), true);
        }
    }
}

// Moves a scene's images, materials, meshes, instances, walls, light maps and object lighting into
// another, its instances moved by `place`.
inline void appendScene(ts2::scene::Scene& out, ts2::scene::Scene add, const ts2::scene::Transform& place) {
    using namespace ts2::scene;
    const int images = static_cast<int>(out.images.size()), materials = static_cast<int>(out.materials.size());
    const int meshes = static_cast<int>(out.meshes.size()), walls = static_cast<int>(out.walls.size());
    const std::size_t instances = out.instances.size();
    const int lightSets = static_cast<int>(out.lightSets.size());
    const int envCubes = static_cast<int>(out.envCubes.size());
    out.envCubes.insert(out.envCubes.end(), std::make_move_iterator(add.envCubes.begin()), std::make_move_iterator(add.envCubes.end()));
    out.images.insert(out.images.end(), std::make_move_iterator(add.images.begin()), std::make_move_iterator(add.images.end()));
    for (Material& m : add.materials) {
        if (m.image >= 0) {
            m.image += images;
        }
        if (m.normalMap >= 0) {
            m.normalMap += images;
        }
        if (m.envCube >= 0) {
            m.envCube += envCubes;
        }
        out.materials.push_back(std::move(m));
    }
    for (Mesh& m : add.meshes) {
        for (auto& p : m.primitives) {
            if (p.material >= 0) {
                p.material += materials;
            }
        }
        out.meshes.push_back(std::move(m));
    }
    for (Instance& i : add.instances) {
        if (i.mesh >= 0) {
            i.mesh += meshes;
        }
        if (i.lightMap >= 0) {
            i.lightMap += images;
        }
        if (i.incidenceMap >= 0) {
            i.incidenceMap += images;
        }
        i.transform = place * i.transform;
        for (auto& w : i.walls) {
            w += walls;
        }
        if (i.lightSet >= 0) {
            i.lightSet += lightSets;
        }
        for (auto& set : i.partLightSets) {
            if (set >= 0) {
                set += lightSets;
            }
        }
        out.instances.push_back(std::move(i));
    }
    for (Actor& a : add.actors) {
        a.place = place * a.place;
        a.lotFrame = place * a.lotFrame;
        for (auto& s : a.skinned) {
            s.instance += instances;
        }
        for (auto& h : a.held) {
            h.instance += instances;
        }
        out.actors.push_back(std::move(a));
    }
    for (ts2::scene::PlacedObject& o : add.placedObjects) {
        o.instance += instances;
        o.lotFrame = place * o.lotFrame;
        out.placedObjects.push_back(std::move(o));
    }
    if (add.floorHeight && !out.floorHeight) {
        out.floorHeight = std::move(add.floorHeight);
        out.groundLevel = add.groundLevel;
    }
    if (add.floorCorner && !out.floorCorner) {
        out.floorCorner = std::move(add.floorCorner);
    }
    if (out.lotSize[0] <= 0) {
        out.lotSize = add.lotSize;
    }
    if (add.lightMaps && !out.lightMaps) {
        auto& maps = out.lightMaps.emplace(std::move(*add.lightMaps));
        for (auto* list : {&maps.floors, &maps.floorIncidence}) {
            for (auto& f : *list) {
                if (f >= 0) {
                    f += images;
                }
            }
        }
        for (auto* one : {&maps.walls, &maps.wallIncidence}) {
            if (*one >= 0) {
                *one += images;
            }
        }
    }
    if (add.objectLighting && !out.objectLighting) {
        placeLights(out.objectLighting.emplace(std::move(*add.objectLighting)), place);
    }
    for (auto& set : add.lightSets) {
        placeLights(out.lightSets.emplace_back(std::move(set)), place);
    }
    if (add.thingSun && !out.thingSun) {
        out.thingSun = std::move(add.thingSun);
    }
    if (add.thingLights && !out.thingLights) {
        out.thingLights = [lights = std::move(add.thingLights), place](const Mesh& mesh, const Transform& inLot, int storey) {
            auto sets = lights(mesh, inLot, storey);
            for (auto& set : sets) {
                placeLights(set, place);
            }
            return sets;
        };
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
inline ts2::scene::Scene lotInNeighborhood(const ts2::scene::Scene& hood, ts2::scene::Scene lot, ts2::scene::Scene skirt,
                                           const ts2::scene::LotFootprint& where, const ts2::scene::SkirtArea& area) {
    using namespace ts2::scene;
    Scene out;
    out.terrain = hood.terrain;
    out.skybox = std::move(lot.skybox);
    out.clip = skirt.clip;
    const float tile = out.terrain ? out.terrain->tileSize : 10.0f;
    const float ax0 = static_cast<float>(area.x0) * tile, ay0 = static_cast<float>(area.y0) * tile;
    const float ax1 = static_cast<float>(area.x1) * tile, ay1 = static_cast<float>(area.y1) * tile;
    // The neighborhood's plants within the area. The plumbobs over lots belong to the neighborhood
    // view only.
    Scene plants = hood;
    plants.terrain.reset();
    plants.objectLighting.reset();  // the lot's lighting state lights everything in lot view
    std::erase_if(plants.instances, [&](const Instance& i) {
        const float x = i.transform.m[3], y = i.transform.m[7];
        return x < ax0 || x > ax1 || y < ay0 || y > ay1 || i.name == ts2::scene::kNeighborhoodPlumbobName;
    });
    appendScene(out, std::move(plants), Transform{});
    appendScene(out, std::move(skirt), Transform{});
    appendScene(out, std::move(lot), where.place);
    if (out.terrain) {
        auto& t = *out.terrain;
        // Other lots' imposters within the area, not this lot's. In lot view the game stands them on
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
