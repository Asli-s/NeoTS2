#include "scene/lot_lighting.hpp"

#include "content/lot.hpp"
#include "diag/log.hpp"
#include "scene/light_corners.hpp"
#include "scene/light_grid.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <set>

namespace ts2::scene::lotlighting {

namespace {

// A state's diffParam by name, else the light files' global setting, else `fallback`.
float diffusionNumber(const content::Lighting& l, const content::Lighting::State& s, const char* param, const char* global, float fallback) {
    if (const auto it = s.diffParams.find(param); it != s.diffParams.end() && !it->second.empty()) {
        return it->second[0];
    }
    return l.number(global, fallback);
}

std::array<float, 3> diffusionColor(const content::Lighting& l, const content::Lighting::State& s, const char* param, const char* global) {
    if (const auto it = s.diffParams.find(param); it != s.diffParams.end() && it->second.size() >= 3) {
        return {it->second[0], it->second[1], it->second[2]};
    }
    std::array<float, 3> c{1, 1, 1};
    if (const auto it = l.values.find(global); it != l.values.end()) {
        // "(r, g, b)": three numbers, each after the bracket or a comma.
        const char* p = it->second.c_str();
        for (auto& v : c) {
            while (*p == ' ' || *p == '(' || *p == ',') ++p;
            char* end = nullptr;
            const float f = std::strtof(p, &end);
            if (end == p) break;
            v = f;
            p = end;
        }
    }
    return c;
}

windowlight::Modifiers modifiersOf(const content::Lighting::State& s, const char* kind) {
    windowlight::Modifiers m;
    if (const auto it = s.modifiers.find(kind); it != s.modifiers.end()) {
        m = {it->second.practicalStrength, it->second.practicalFalloff, it->second.diffusionStrength, it->second.diffusionFalloff};
    }
    return m;
}

// A room light setting for a group ("object:roomOverallStrength"), else its global value.
float groupNumber(const content::Lighting& l, const std::string& group, const std::string& name, float fallback) {
    return l.number(group + name, l.number(name, fallback));
}

roomlight::Settings groupSettings(const content::Lighting& l, const std::string& group) {
    roomlight::Settings s;
    s.type = static_cast<int>(groupNumber(l, group, "roomLightingType", 4));
    s.overall = groupNumber(l, group, "roomOverallStrength", s.overall);
    s.bounce = groupNumber(l, group, "roomBounceStrength", s.bounce);
    s.bounceCrossover = groupNumber(l, group, "roomBounceCrossover", s.bounceCrossover);
    s.side = groupNumber(l, group, "roomSideStrength", s.side);
    s.sideCrossover = groupNumber(l, group, "roomSideCrossover", s.sideCrossover);
    s.sideSlope = groupNumber(l, group, "roomSideSlope", s.sideSlope);
    s.specular = groupNumber(l, group, "roomSpecular", s.specular);
    s.fromLightMapMix = groupNumber(l, group, "roomFromLightMapMix", s.fromLightMapMix);
    s.fromLightMapStrength = groupNumber(l, group, "roomFromLightMapStrength", s.fromLightMapStrength);
    s.directionalBoost = groupNumber(l, group, "roomDirectionalBoost", s.directionalBoost);
    return s;
}

// A level's floor light grid, null past the lot's levels.
const floorlight::Grid* gridOf(const Result& r, int level) {
    const int i = level - r.lowest;
    return i >= 0 && i < static_cast<int>(r.floors.size()) ? &r.floors[static_cast<std::size_t>(i)] : nullptr;
}

// The room a point stands in, as the floor light grid of its level has it.
std::int16_t gridRoom(const Result& r, int level, float x, float y) {
    const auto* grid = gridOf(r, level);
    if (!grid || !std::isfinite(x) || !std::isfinite(y)) {
        return 0;
    }
    const auto& g = *grid;
    if (!(x >= 0 && y >= 0 && x < static_cast<float>(g.width) && y < static_cast<float>(g.height))) {
        return 0;
    }
    const int cx = static_cast<int>(std::floor(x)), cy = static_cast<int>(std::floor(y));
    const auto& tile = g.tiles[static_cast<std::size_t>(cy) * static_cast<std::size_t>(g.width) + static_cast<std::size_t>(cx)];
    const float fx = x - static_cast<float>(cx) - 0.5f, fy = y - static_cast<float>(cy) - 0.5f;
    const bool first = tile.diagonal == 0 || (tile.diagonal == 1 && fy < -fx) || (tile.diagonal == 2 && fy < fx);
    const auto* cell = g.at(2 * cx + (fx >= 0 ? 1 : 0), 2 * cy + (fy >= 0 ? 1 : 0));
    if (!cell) {
        return 0;
    }
    const auto& half = cell->halves[first ? 0 : 1];
    return half.valid ? half.room : 0;
}

bool tileHasFloor(const Result& r, int level, int x, int y) {
    const auto* grid = gridOf(r, level);
    if (!grid) {
        return false;
    }
    const auto& g = *grid;
    if (x < 0 || y < 0 || x >= g.width || y >= g.height) {
        return false;
    }
    return g.tiles[static_cast<std::size_t>(y) * static_cast<std::size_t>(g.width) + static_cast<std::size_t>(x)].floor;
}

// One pass of the floor and wall light with these sources. False when a pass hit its safety limit.
bool lightPass(Result& r, const Settings& s, const std::vector<LightSource>& sources) {
    bool ok = true;
    for (auto& g : r.floors) {
        g.insideAmbient = {s.insideColor[0] * s.insideFloorAmbient, s.insideColor[1] * s.insideFloorAmbient, s.insideColor[2] * s.insideFloorAmbient};
        g.outsideAmbient = {s.outsideColor[0] * s.outsideFloorAmbient, s.outsideColor[1] * s.outsideFloorAmbient,
                            s.outsideColor[2] * s.outsideFloorAmbient};
        g.falloffScale = s.floorThreshold;
        g.steps = s.steps;
        g.dimOutside = s.dimOutside;
        ok = floorlight::light(g, sources, [&](int x, int y, int level) { return tileHasFloor(r, level, x / 2, y / 2); }) && ok;
    }
    auto& w = r.walls;
    w.insideAmbient = {s.insideColor[0] * s.insideWallAmbient, s.insideColor[1] * s.insideWallAmbient, s.insideColor[2] * s.insideWallAmbient};
    w.outsideAmbient = {s.outsideColor[0] * s.outsideWallAmbient, s.outsideColor[1] * s.outsideWallAmbient, s.outsideColor[2] * s.outsideWallAmbient};
    w.falloffScale = s.wallThreshold;
    w.steps = s.steps;
    w.brightFloor = s.brightWallFloor;
    w.darkFloor = s.darkWallFloor;
    ok = walllight::light(
             w, sources,
             [&](float x, float y, int level) {
                 return std::isfinite(x) && std::isfinite(y) && tileHasFloor(r, level, static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)));
             },
             [&](float x, float y, int level) {
                 const auto* grid = gridOf(r, level);
                 return grid ? floorlight::sample(*grid, x, y) : std::array<float, 3>{0, 0, 0};
             }) &&
         ok;
    return ok;
}

wallruns::Key portalKey(const windowlight::Portal& p) {
    return wallruns::keyOf(static_cast<int>(p.height), static_cast<int>(std::lround(p.x0)), static_cast<int>(std::lround(p.y0)),
                           static_cast<int>(std::lround(p.x1)), static_cast<int>(std::lround(p.y1)));
}

} // namespace

std::optional<Settings> settingsFor(const content::Lighting& l, const std::string& name) {
    const auto it = l.states.find(name);
    if (it == l.states.end()) {
        return std::nullopt;
    }
    const auto& st = it->second;
    Settings s;
    s.insideFloorAmbient = diffusionNumber(l, st, "insideFloorAmbient", "insideFloorAmbient", 0);
    // The game takes only these from a state. The outside ambients always come from the global settings.
    s.outsideFloorAmbient = l.number("outsideFloorAmbient", 0);
    s.insideWallAmbient = diffusionNumber(l, st, "insideWallAmbient", "insideWallAmbient", 0);
    s.outsideWallAmbient = l.number("outsideWallAmbient", 0);
    s.insideColor = diffusionColor(l, st, "insideAmbientColor", "insideDiffusionAmbientColor");
    s.outsideColor = diffusionColor(l, content::Lighting::State{}, "", "outsideDiffusionAmbientColor");
    s.floorThreshold = diffusionNumber(l, st, "floorThreshold", "floorDiffusionThreshold", s.floorThreshold);
    s.wallThreshold = diffusionNumber(l, st, "wallThreshold", "wallDiffusionThreshold", s.wallThreshold);
    // Clamped first, because a modded value past int's range can't be cast.
    s.steps = static_cast<int>(std::clamp(l.number("wallPropagationSteps", 50), -1.0e6f, 1.0e6f));
    s.brightWallFloor = l.number("brightWallFloorContribution", s.brightWallFloor);
    s.darkWallFloor = l.number("darkWallFloorContribution", s.darkWallFloor);
    s.incidenceScale = l.number("incidenceMapScale", s.incidenceScale);
    s.incidenceHeightClamp = l.number("incidenceMapHeightClamp", s.incidenceHeightClamp);

    if (const auto sun = st.lights.find("sun"); sun != st.lights.end()) {
        const auto& d = sun->second.direction;
        const double length = std::sqrt(static_cast<double>(d[0]) * d[0] + static_cast<double>(d[1]) * d[1] + static_cast<double>(d[2]) * d[2]);
        if (length > 0) {
            s.sun = true;
            s.day.sunColor = sun->second.color;
            s.day.sunStrength = sun->second.strength;
            for (std::size_t c = 0; c < 3; ++c) s.day.sunDirection[c] = static_cast<float>(d[c] / length);
        }
    }
    s.day.sky = st.skylight;
    // The outdoor lights the light maps take, the sun first.
    for (const char* lightName : {"sun", "sky", "fill1"}) {
        const auto light = st.lights.find(lightName);
        if (light == st.lights.end()) continue;
        const auto& d = light->second.direction;
        const double length = std::sqrt(static_cast<double>(d[0]) * d[0] + static_cast<double>(d[1]) * d[1] + static_cast<double>(d[2]) * d[2]);
        if (!(length > 0)) continue;
        s.outdoor.lights.push_back({light->second.color, light->second.strength,
                                    {static_cast<float>(d[0] / length), static_cast<float>(d[1] / length), static_cast<float>(d[2] / length)}});
    }
    s.outdoor.skylight = st.skylight;
    s.outdoor.strength = l.number("terrainStrength", 1);
    s.day.insidePortals = modifiersOf(st, "insidePortals");
    s.day.outsidePortals = modifiersOf(st, "outsidePortals");
    s.day.insidePortalSpills = modifiersOf(st, "insidePortalSpills");
    s.day.outsidePortalSpills = modifiersOf(st, "outsidePortalSpills");
    s.day.doorOrArchDirect = l.number("doorOrArchDirectMultiplier", 0);
    s.suppression.radius = l.number("portalSuppressionRadius", s.suppression.radius);
    s.suppression.facing = l.number("portalSuppressionAmount", s.suppression.facing);
    s.suppression.corner = l.number("portalCornerSuppressionAmount", s.suppression.corner);

    s.samples.saturation = l.number("roomSaturation", s.samples.saturation);
    s.samples.ceiling = l.number("roomCeilingStrength", s.samples.ceiling);
    s.samples.portal = l.number("roomPortalStrength", s.samples.portal);
    s.groups = {groupSettings(l, "object:"), groupSettings(l, "sim:"), groupSettings(l, "wall:")};
    if (st.unlitRoom) {
        s.unlit = *st.unlitRoom;
    }
    return s;
}

Result compute(const Inputs& in, const Settings& s) {
    Result r;
    if (!in.lot) {
        return r;
    }
    const auto& lot = *in.lot;
    const auto floorGrid = lot.grids.find(content::kFloorGridInstance);
    if (floorGrid == lot.grids.end()) {
        return r;
    }
    const int levels = static_cast<int>(std::min<std::uint32_t>(floorGrid->second.levels, 16));
    if (floorGrid->second.levels > 16) {
        diag::warnOnce("lot lighting: levels past 16 are left unlit");
    }
    r.lowest = -std::clamp(content::groundGridLevel(lot), 0, levels);

    // The floor light grids and the wall faces, rooms as the grids see them.
    const auto walls = wallruns::wallsOf(lot, in.blocksLight);
    for (int level = r.lowest; level < r.lowest + levels; ++level) {
        auto li = lightgrid::fromLot(lot, level, walls);
        if (in.roomAt) {
            li.roomAt = [&in, level](float x, float y) { return in.roomAt(level, x, y); };
        }
        r.floors.push_back(lightgrid::build(li));
    }
    r.layout = wallruns::build(walls, [&](int level, float x, float y) { return gridRoom(r, level, x, y); }, wallruns::floorSolidOf(lot));
    r.walls.runs = r.layout.runs;
    r.complete = r.layout.complete;

    // The lamps alone. The windows and doors take their light from those walls.
    r.complete = lightPass(r, s, in.lamps) && r.complete;
    const std::array<float, 3> outsideWall = s.outsideColor;
    std::map<std::int16_t, std::vector<std::size_t>> byRoom;
    for (std::size_t i = 0; i < in.portals.size(); ++i) byRoom[in.portals[i].room].push_back(i);
    std::vector<LightSource> sources = in.lamps;
    for (const auto& [room, list] : byRoom) {
        std::vector<windowlight::Portal> portals;
        for (const auto i : list) portals.push_back(in.portals[i].portal);
        windowlight::suppress(portals, s.suppression);
        for (std::size_t k = 0; k < portals.size(); ++k) {
            PortalLight pl;
            pl.into = portals[k].otherRoom;
            pl.portal = portals[k];
            pl.spillPoint = in.portals[list[k]].spillPoint;
            pl.lightPosition = in.portals[list[k]].lightPosition;
            pl.lightAxis = in.portals[list[k]].lightAxis;
            pl.spot = in.portals[list[k]].spot;
            pl.coneAngle = in.portals[list[k]].coneAngle;
            pl.conePenumbra = in.portals[list[k]].conePenumbra;
            const auto L = wallruns::portalSample(r.walls.runs, r.layout, portalKey(portals[k]), s.dimOutside, outsideWall, s.outsideWallAmbient);
            pl.lights = windowlight::lightsFor(portals[k], room == 0, s.sun, s.day, L);
            if (pl.lights) {
                sources.insert(sources.end(), pl.lights->sources.begin(), pl.lights->sources.end());
            }
            r.portals.push_back(std::move(pl));
        }
    }

    // Lamps and windows together, then the corners.
    r.walls.runs = r.layout.runs;
    r.complete = lightPass(r, s, sources) && r.complete;
    lightcorners::blendWalls(r.walls);
    std::vector<std::vector<lightcorners::Tile>> tiles;
    for (const auto& keys : r.layout.keys) {
        tiles.emplace_back();
        for (const auto& k : keys) tiles.back().push_back({k.x, k.y, k.level});
    }
    const auto corners = lightcorners::corners(r.walls, tiles);
    for (auto& g : r.floors) lightcorners::blendFloor(g, corners);

    // Each room's samples and lights.
    roomsamples::LotInputs si;
    si.quarterRoom = in.quarterRoom;
    si.faceRoom = [&](const wallruns::Key& k) {
        return wallruns::roomInFront(k, [&](int level, float x, float y) { return in.roomAt ? in.roomAt(level, x, y) : std::int16_t{0}; });
    };
    si.covering = in.covering;
    si.floorLight = [&](int level, float x, float y) {
        const auto* grid = gridOf(r, level);
        return grid ? floorlight::sample(*grid, x, y) : roomsamples::Color{0, 0, 0};
    };
    si.wallLight = [&](const wallruns::Key& k) { return wallruns::portalSample(r.walls.runs, r.layout, k, false, {0, 0, 0}, 0); };
    std::vector<roomsamples::PortalInput> portalInputs;
    for (const auto& p : r.portals) {
        if (p.lights) portalInputs.push_back({p.into, p.portal.nx, p.portal.ny, p.lights->roomColor});
    }
    std::set<std::int16_t> litBySource;
    for (const auto& src : sources) litBySource.insert(src.room);
    auto gathered = roomsamples::gather(lot, si, portalInputs, s.samples);
    for (int id = 1; id < std::min(in.roomCount, 4096); ++id) {
        gathered[static_cast<std::int16_t>(id)];  // rooms nothing reaches still get the unlit room light
    }
    for (auto& [room, samples] : gathered) {
        if (room <= 0) {
            continue;
        }
        Room out;
        out.samples = samples.averages();
        out.unlitShare = roomlight::unlitShare(out.samples, s.unlit.color, s.unlit.threshold, litBySource.count(room) != 0);
        roomlight::addUnlitLight(out.samples, s.unlit.color, s.unlit.threshold, litBySource.count(room) != 0);
        for (std::size_t g = 0; g < 3; ++g) out.groups[g] = roomlight::lightsFor(out.samples, s.groups[g]);
        out.average = roomlight::average(out.samples);
        r.rooms[room] = out;
    }
    return r;
}

} // namespace ts2::scene::lotlighting
