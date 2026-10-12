#include "scene/lot_light_objects.hpp"

#include "content/lot.hpp"
#include "formats/dbpf.hpp"
#include "formats/objd.hpp"
#include "formats/resource_types.hpp"
#include "formats/scenegraph.hpp"
#include "scene/assets.hpp"
#include "scene/lamp_lights.hpp"
#include "scene/lot_parts.hpp"
#include "scene/wall_runs.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace ts2::scene::lotlightobjects {

namespace {

namespace sg = scenegraph;

std::string lowered(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Object kinds (the object definition's type) whose lights are portals: doors and windows.
constexpr int kDoorKind = 8, kWindowKind = 9;

// A model's resource tree and its light resources by name, read once per model.
struct Model {
    sg::ResourceNode tree;
    std::map<std::string, sg::LightResource> lights;
};

const Model* modelOf(Assets& assets, std::map<std::string, std::optional<Model>>& cache, const std::string& name) {
    const auto found = cache.find(name);
    if (found != cache.end()) {
        return found->second ? &*found->second : nullptr;
    }
    auto& slot = cache[name];
    const auto cres = assets.byName(0xE519C933, name + "_cres");
    if (!cres) {
        return nullptr;
    }
    Model m;
    try {
        m.tree = sg::parseResourceNode(*cres);
    } catch (const std::exception&) {
        return nullptr;
    }
    for (const auto& l : m.tree.container.links) {
        if ((l.type & 0xFFFFFF00u) != 0xC9C81B00u) {
            continue;  // not a light resource
        }
        if (const auto data = assets.byKey(l)) {
            try {
                auto r = sg::parseLight(*data);
                m.lights[lowered(r.name)] = r;
            } catch (const std::exception&) {
            }
        }
    }
    slot = std::move(m);
    return &*slot;
}

} // namespace

Found gather(Assets& assets, const content::Lot& lot, const std::filesystem::path& lotPackage, const content::Lighting::State& state,
             const Queries& queries) {
    Found out;
    const auto floors = lot.grids.find(content::kFloorGridInstance);
    if (floors == lot.grids.end()) {
        return out;
    }
    const auto& lighting = assets.lighting();
    const Heights heights(lot);
    const int width = static_cast<int>(floors->second.width), height = static_cast<int>(floors->second.height);
    const int levels = static_cast<int>(floors->second.levels);
    auto roomAt = [&](float x, float y, int level) { return queries.roomAt ? queries.roomAt(level, x, y) : std::int16_t{0}; };

    lamplights::LotQueries lamps;
    lamps.lowestLevel = 0;
    lamps.highestLevel = levels - 1;
    lamps.floorHeight = [&](int level, float x, float y) { return heights.corner(level, std::nearbyint(x - 0.5f), std::nearbyint(y - 0.5f)); };
    lamps.tileOnLot = [&](int x, int y, int level) { return x >= 0 && y >= 0 && level >= 0 && x < width && y < height && level < levels; };
    lamps.roomAt = roomAt;

    // Wall edges by level, both ways round, for the portals.
    std::set<std::tuple<int, int, int, int, int>> edges;
    for (const auto& w : wallruns::wallsOf(lot, nullptr)) {
        const int dx = (w.x1 > w.x0) - (w.x1 < w.x0), dy = (w.y1 > w.y0) - (w.y1 < w.y0);
        const int n = std::max(std::abs(w.x1 - w.x0), std::abs(w.y1 - w.y0));
        for (int i = 0; i < n; ++i) {
            const int ax = w.x0 + i * dx, ay = w.y0 + i * dy;
            edges.insert({w.level, ax, ay, ax + dx, ay + dy});
            edges.insert({w.level, ax + dx, ay + dy, ax, ay});
        }
    }
    auto ints = [](const portalrecords::Edge& e) {
        return std::array<int, 4>{static_cast<int>(std::lround(e.x0)), static_cast<int>(std::lround(e.y0)), static_cast<int>(std::lround(e.x1)),
                                  static_cast<int>(std::lround(e.y1))};
    };
    portalrecords::LotQueries portals;
    portals.lamps = lamps;
    portals.inLot = [&](float x, float y) { return x >= 0 && y >= 0 && x < static_cast<float>(width) && y < static_cast<float>(height); };
    portals.wallAt = [&](const portalrecords::Edge& e) {
        const auto i = ints(e);
        return edges.count({e.level, i[0], i[1], i[2], i[3]}) != 0;
    };
    portals.roomInFront = [&](const portalrecords::Edge& e) {
        const auto i = ints(e);
        return wallruns::roomInFront(wallruns::keyOf(e.level, i[0], i[1], i[2], i[3]),
                                     [&](int level, float x, float y) { return roomAt(x, y, level); });
    };
    portals.roomLit = queries.roomLit;
    const float spillOffset = lighting.number("spillPlaneOffset", 0.3f);
    const auto inside = state.modifiers.find("insideObjects"), outside = state.modifiers.find("outsideObjects");
    const content::Lighting::Modifier none{};

    dbpf::Package package{lotPackage};
    std::map<std::uint32_t, const dbpf::IndexEntry*> records;
    for (const auto& e : package.entries()) {
        if (e.key.type == types::kSceneObject) records[e.key.instance] = &e;
    }
    std::map<std::string, std::optional<Model>> models;
    for (const auto& object : lot.objects) {
        const auto rec = records.find(object.id);
        if (rec == records.end()) {
            continue;
        }
        sg::SceneObject so;
        try {
            so = sg::parseSceneObject(package.read(*rec->second));
        } catch (const std::exception&) {
            continue;
        }
        const Model* model = so.model.empty() ? nullptr : modelOf(assets, models, so.model);
        if (!model) {
            continue;
        }
        const auto& h = object.state.header;
        const float x = h.location[1], y = h.location[0];
        if (!(x >= 0 && y >= 0 && x <= 256 && y <= 256)) {
            continue;
        }
        const float z = heights.tile(h.value1 - 1, x, y) + h.value3;
        const auto nodes = lamplights::lightNodes(model->tree, {{x, y, z}, lamplights::facingRotation(object.state.data[1])});
        if (nodes.empty()) {
            continue;
        }
        const auto* objd = object.type ? assets.objectDefinition(object.type->guid) : nullptr;
        const int kind = objd ? objd->field(objd::field::kType) : -1;

        if (kind != kDoorKind && kind != kWindowKind) {
            const float strength = queries.strength ? queries.strength(object.id) : 1.0f;
            for (const auto& node : nodes) {
                const auto& p = node.place.position;
                const auto room = roomAt(p[0], p[1], lamplights::levelAt(lamps, p[0], p[1], p[2]));
                if (queries.roomLit && !queries.roomLit(room)) {
                    continue;
                }
                const auto& mod = room != 0 ? (inside != state.modifiers.end() ? inside->second : none)
                                            : (outside != state.modifiers.end() ? outside->second : none);
                const auto settings = lamplights::scaled(lamplights::settingsFor(lighting.light(node.name)), mod, strength);
                if (const auto s = lamplights::sourceFor(settings, node.place, lamps)) {
                    out.lamps.push_back(*s);
                }
            }
            continue;
        }

        portalrecords::PortalObject po;
        po.doorOrArch = kind == kDoorKind;
        for (const auto& node : nodes) {
            portalrecords::PortalLight pl;
            pl.place = node.place;
            const auto* attrs = lighting.light(node.name);
            pl.settings = lamplights::settingsFor(attrs);
            const auto res = model->lights.find(lowered(node.name));
            pl.hasLight = res != model->lights.end();
            if (pl.hasLight) {
                pl.intensity = attrs && attrs->intensity ? *attrs->intensity : res->second.intensity;
                pl.color = attrs && attrs->color ? *attrs->color : res->second.color;
                pl.falloffRate = attrs && attrs->falloffRate ? *attrs->falloffRate : res->second.falloffRate;
            }
            po.lights.push_back(pl);
        }
        // The wall edge nearest the first light, on the object's level.
        const int level = h.value1 - 1;
        const auto& p0 = nodes[0].place.position;
        float best = 1e9f;
        for (const auto& [l, ax, ay, bx, by] : edges) {
            if (l != level) continue;
            const float d = std::hypot((ax + bx) * 0.5f - p0[0], (ay + by) * 0.5f - p0[1]);
            if (d < best) {
                best = d;
                po.edge = {static_cast<float>(ax), static_cast<float>(ay), level, static_cast<float>(bx), static_cast<float>(by)};
            }
        }
        if (best > 1.0f) {
            continue;
        }
        for (auto& b : portalrecords::build(po, portals, spillOffset)) out.portals.push_back(std::move(b));
    }
    return out;
}

} // namespace ts2::scene::lotlightobjects
