#include "scene/lamp_lights.hpp"

#include "diag/log.hpp"
#include "scene/portable_math.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>

// The game works these out on the x87 in double precision and rounds to float only where it
// stores a value, so the code below does the same: double in between, float where the game
// keeps a float.

namespace ts2::scene::lamplights {

namespace {

constexpr int kMaxLevels = 64;

float r24(double v) {
    return static_cast<float>(v);
}

// A colour channel the game counts as zero: within 32 steps of float precision of 0.
bool nearZero(float v) {
    const auto bits = std::bit_cast<std::int32_t>(v);
    const std::int64_t ordered = bits >= 0 ? bits : -static_cast<std::int64_t>(bits & 0x7FFFFFFF);
    return ordered >= -32 && ordered <= 31;
}


// The area a light evenly lights: its own area turned to face the light's way (its +y along the
// facing), around the light. The game turns two corners and takes their smaller and larger
// coordinates.
void placeArea(LightSource& out, const Settings& s, float x, float y) {
    const double turn = static_cast<double>(out.angle) - static_cast<double>(std::bit_cast<float>(0x3FC90FDBu));
    const double sn = portable::sine(turn), cs = portable::cosine(turn);
    const float ax = r24(cs * s.min[0] - sn * s.min[1]);
    const float ay = r24(sn * s.min[0] + cs * s.min[1]);
    const float bx = r24(cs * s.max[0] - sn * s.max[1]);
    const double byWide = sn * s.max[0] + cs * s.max[1];
    const float by = r24(byWide);
    const float xMin = ax <= bx ? ax : bx;
    const float yMin = static_cast<double>(ay) <= byWide ? ay : by;
    const float xMax = bx <= ax ? ax : bx;
    const float yMax = by <= ay ? ay : by;
    out.x0 = xMin + x;
    out.y0 = yMin + y;
    out.x1 = xMax + x;
    out.y1 = yMax + y;
}

using Quaternion = std::array<float, 4>;

// p times q (p on the left), rounded to float and normalized, in the game's order of operations.
Quaternion timesNormalized(const Quaternion& p, const Quaternion& q) {
    auto m = [](float a, float b) { return static_cast<double>(a) * b; };
    const float x = r24(((m(p[0], q[3]) + m(p[1], q[2])) + m(p[3], q[0])) - m(p[2], q[1]));
    const float y = r24(((m(p[1], q[3]) + m(p[3], q[1])) + m(p[2], q[0])) - m(p[0], q[2]));
    const float z = r24(((m(p[2], q[3]) + m(p[3], q[2])) + m(p[0], q[1])) - m(p[1], q[0]));
    const float w = r24(((m(p[3], q[3]) - m(p[0], q[0])) - m(p[1], q[1])) - m(p[2], q[2]));
    const double scale = 1.0 / std::sqrt(((m(x, x) + m(y, y)) + m(z, z)) + m(w, w));
    return {r24(x * scale), r24(y * scale), r24(z * scale), r24(w * scale)};
}

// A node's place in the world from its parent's and its own: turned by the parent's rotation,
// then moved to its position.
Placement within(const Placement& parent, const Placement& local) {
    const auto at = turned(parent.rotation, local.position);
    return {{at[0] + parent.position[0], at[1] + parent.position[1], at[2] + parent.position[2]}, timesNormalized(parent.rotation, local.rotation)};
}

constexpr int kMaxTreeDepth = 64;

} // namespace

// Turns v by the unit quaternion q (x, y, z, w), in the game's order of operations.
std::array<float, 3> turned(const std::array<float, 4>& q, const std::array<float, 3>& v) {
    auto twice = [](float p, float r) { return static_cast<double>(p) * r * 2.0; };
    const double a = twice(q[0], q[0]), b = twice(q[1], q[0]);
    const float c = r24(twice(q[2], q[0])), d = r24(twice(q[3], q[0]));
    const float e = r24(twice(q[1], q[1])), f = r24(twice(q[2], q[1])), g = r24(twice(q[3], q[1]));
    const double h = twice(q[2], q[2]), i = twice(q[2], q[3]);
    const float x = r24(((1.0 - e) - h) * v[0] + (b - i) * v[1] + (static_cast<double>(g) + c) * v[2]);
    const float s = r24(1.0 - a);
    const float y = r24((((1.0 - a) - h) * v[1] + (static_cast<double>(f) - d) * v[2]) + (i + b) * v[0]);
    const float z = r24(((static_cast<double>(s) - e) * v[2] + (static_cast<double>(c) - g) * v[0]) + (static_cast<double>(f) + d) * v[1]);
    return {x, y, z};
}

std::vector<LightNode> lightNodes(const scenegraph::ResourceNode& model, const Placement& object, std::span<const cobject::SavedJoint> saved) {
    std::vector<LightNode> out;
    const auto& nodes = model.nodes;
    auto sameName = [](const std::string& a, const std::string& b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    };
    auto finite = [](const auto& values) { return std::ranges::all_of(values, [](float v) { return std::isfinite(v); }); };
    auto placeOf = [&](const scenegraph::SceneNode& node) -> Placement {
        for (const auto& j : saved) {
            if (!node.name.empty() && sameName(j.name, node.name) && finite(j.translation) && finite(j.rotation)) {
                return {j.translation, j.rotation};
            }
        }
        return {node.translation, node.rotation};
    };
    std::vector<bool> seen(nodes.size(), false);
    auto walk = [&](auto&& self, std::uint32_t index, const Placement& parent, int depth) -> void {
        if (index >= nodes.size() || seen[index] || depth > kMaxTreeDepth) {
            if (depth > kMaxTreeDepth) {
                diag::warnOnce("lamp lights: lights nested deeper than 64 levels in a model are dropped");
            }
            return;
        }
        seen[index] = true;
        const auto& node = nodes[index];
        const Placement here = within(parent, placeOf(node));
        if (node.kind == scenegraph::SceneNode::Kind::LightRef && !node.name.empty()) {
            out.push_back({node.name, here});
        }
        for (const auto child : node.children) {
            self(self, child, here, depth + 1);
        }
    };
    if (!nodes.empty()) {
        walk(walk, 0, within(Placement{}, object), 0);
    }
    return out;
}

std::array<float, 4> facingRotation(int direction) {
    // The angle in float steps of pi/4 from +y, wrapped into [0, 2 pi) before halving. The game
    // keeps the sign this gives (w turns negative past half a turn).
    const float quarter = std::bit_cast<float>(0x3F490FDBu), turn = std::bit_cast<float>(0x40C90FDBu);
    const float angle = r24(static_cast<double>(static_cast<std::int16_t>(2 - direction)) * quarter);
    const double half = std::fmod(static_cast<double>(angle) + turn, static_cast<double>(turn)) * 0.5;
    return {0.0f, 0.0f, r24(portable::sine(half)), r24(portable::cosine(half))};
}

Settings settingsFor(const content::Lighting::LightAttributes* a) {
    Settings s;
    if (!a) {
        return s;
    }
    s.min = a->diffusionMin.value_or(s.min);
    s.max = a->diffusionMax.value_or(s.max);
    s.intensity = a->diffusionIntensity.value_or(s.intensity);
    s.color = a->diffusionColor.value_or(s.color);
    s.directional = a->diffusionDirectional.value_or(s.directional);
    s.falloff = a->diffusionFalloff.value_or(s.falloff);
    s.directionalFalloff = a->diffusionDirectionalFalloff.value_or(s.directionalFalloff);
    s.wallScale = a->diffusionWallScale.value_or(s.wallScale);
    s.floorScale = a->diffusionFloorScale.value_or(s.floorScale);
    s.interFloorFalloff = a->diffusionInterFloorFalloff.value_or(s.interFloorFalloff);
    return s;
}

Settings scaled(Settings s, const content::Lighting::Modifier& modifier, float strength) {
    s.intensity = r24(static_cast<double>(modifier.diffusionStrength) * s.intensity * strength);
    s.falloff = modifier.diffusionFalloff * s.falloff;
    return s;
}

int levelAt(const LotQueries& lot, float x, float y, float z) {
    const int low = std::max(lot.lowestLevel, -kMaxLevels), high = std::min(lot.highestLevel, kMaxLevels);
    for (int level = high; level >= low && lot.floorHeight; --level) {
        if (static_cast<double>(z) + static_cast<double>(0.01f) > lot.floorHeight(level, x, y)) {
            return level;
        }
    }
    return low;
}

std::optional<LightSource> sourceFor(const Settings& s, const Placement& p, const LotQueries& lot) {
    LightSource out;
    out.color = {r24(static_cast<double>(s.color[0]) * s.intensity), r24(static_cast<double>(s.color[1]) * s.intensity),
                 r24(static_cast<double>(s.color[2]) * s.intensity)};
    if (nearZero(out.color[0]) && nearZero(out.color[1]) && nearZero(out.color[2])) {
        return std::nullopt;
    }
    const float x = p.position[0], y = p.position[1], z = p.position[2];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || std::fabs(x) > 1e6f || std::fabs(y) > 1e6f) {
        return std::nullopt;
    }
    out.level = levelAt(lot, x, y, z);
    // The game truncates toward zero for the tile.
    if (!lot.tileOnLot || !lot.tileOnLot(static_cast<int>(x), static_cast<int>(y), out.level)) {
        return std::nullopt;
    }
    out.directional = s.directional;
    out.directionalMix = s.directionalFalloff;
    out.falloff = s.falloff;
    out.wallIntensity = s.wallScale;
    out.floorIntensity = s.floorScale;
    out.interFloorFalloff = s.interFloorFalloff;

    // It faces along the node's -z, or its +y when -z points (nearly) straight up or down.
    auto facing = turned(p.rotation, {-0.0f, -0.0f, -1.0f});
    if (facing[2] < -0.99 || facing[2] > 0.99) {
        facing = turned(p.rotation, {0.0f, 1.0f, 0.0f});
    }
    const double fx = facing[0], fy = facing[1];
    out.angle = r24(std::atan2(fy, fx));

    // The room is taken in front of lights whose area lies wholly ahead of them.
    const double reach = 0.0f < s.min[1] ? s.min[1] : 0.0f;
    const double k = reach / (fy * fy + fx * fx);
    float rx = r24(x + fx * k), ry = r24(y + static_cast<double>(r24(fy * k)));
    if (!std::isfinite(rx) || !std::isfinite(ry)) {
        rx = x;
        ry = y;
    }
    out.room = lot.roomAt ? lot.roomAt(rx, ry, levelAt(lot, rx, ry, z)) : std::int16_t{0};
    placeArea(out, s, x, y);
    // Broken rotations or areas give no usable light.
    if (!std::isfinite(out.angle) || !std::isfinite(out.x0) || !std::isfinite(out.y0) || !std::isfinite(out.x1) || !std::isfinite(out.y1)) {
        return std::nullopt;
    }
    return out;
}

} // namespace ts2::scene::lamplights
