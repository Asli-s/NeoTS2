#include "scene/particle_quads.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::scene {

namespace {

using Block = effects::ParticleBlock;
using Vec3 = std::array<float, 3>;

constexpr float kTwoPi = 6.28318530717958647692f;

// A layer past this is a broken file's. The game's files go up to 9001.
constexpr float kHighestLayer = 100000;

Vec3 times(const Vec3& v, float s) { return {v[0] * s, v[1] * s, v[2] * s}; }
Vec3 plus(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 minus(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
float dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }

Vec3 turned(const Vec3& v, const std::array<float, 9>& r) {
    return {r[0] * v[0] + r[1] * v[1] + r[2] * v[2], r[3] * v[0] + r[4] * v[1] + r[5] * v[2], r[6] * v[0] + r[7] * v[1] + r[8] * v[2]};
}

// The list's value at `t`, blended between its evenly spaced entries. `fallback` for an empty list.
Vec3 sample(const std::vector<Vec3>& list, float t, const Vec3& fallback) {
    if (list.empty()) {
        return fallback;
    }
    const float x = static_cast<float>(list.size() - 1) * std::clamp(t, 0.0f, 1.0f);
    const auto i = static_cast<std::size_t>(x);
    const float part = x - static_cast<float>(i);
    if (part <= 0 || i + 1 >= list.size()) {
        return list[std::min(i, list.size() - 1)];
    }
    return plus(list[i], times(minus(list[i + 1], list[i]), part));
}

// Up to 255, as 256ths and kept in range.
std::uint8_t byteOf(float v) {
    return static_cast<std::uint8_t>(std::clamp(std::lround(v * 256.0f), 0L, 255L));
}

// A sprite's starting right and up, before its size. Sprites with no alignment of their own face the camera.
void lineUp(const Block& b, const EmitterPlace& own, const EmitterPlace& parent, const ParticleView& view, Vec3& right, Vec3& up) {
    if (b.alignment == Block::Ground || b.alignment == Block::Source) {
        // Along the direction particles leave in, seen from above. A source sprite takes it before the
        // places turn it, then is turned with them, so it can lean out of the flat.
        const Vec3 middle = times(plus(b.directionLow, b.directionHigh), 0.5f);
        const Vec3 flat = b.alignment == Block::Ground ? turned(turned(middle, own.rotation), parent.rotation) : middle;
        const float length = std::hypot(flat[0], flat[1]);
        if (length <= 1e-4f) {
            right = {1, 0, 0};
            up = {0, 1, 0};
        } else {
            up = {flat[0] / length, flat[1] / length, 0};
            right = cross(up, {0, 0, 1});
        }
        if (b.alignment == Block::Source) {
            right = turned(turned(right, own.rotation), parent.rotation);
            up = turned(turned(up, own.rotation), parent.rotation);
        }
    } else if (b.alignment == Block::ZPole) {
        up = turned(turned({0, 0, 1}, own.rotation), parent.rotation);
        const Vec3 side = cross(view.forward, up);
        right = times(side, 1 / (std::sqrt(dot(side, side)) + 0.1f));
    } else {
        right = view.right;
        up = {view.up[0], view.up[1], std::abs(view.up[2])};
    }
}

} // namespace

int particleDrawLayer(const Block& block) {
    const int layer = static_cast<int>(std::lround(std::clamp(block.layer, -kHighestLayer, kHighestLayer)));
    const int material = (block.flags & Block::kDirectLayer) ? layer : layer * 8 + 7;
    return material > 0 ? material + 10 : material;
}

void appendParticleQuads(const Block& b, const ParticleEmitter& emitter, const EmitterPlace& parent, const ParticleView& view, std::vector<ParticleVertex>& out) {
    Vec3 startRight{}, startUp{};
    lineUp(b, emitter.where(), parent, view, startRight, startUp);
    const bool fades = b.screenEdge[0] != 0 || b.screenEdge[2] != 0;
    const float alphaSlope = static_cast<float>(b.screenEdge[0]) * 16 / 255, alphaBase = static_cast<float>(b.screenEdge[1]) / 255;
    const float sizeSlope = static_cast<float>(b.screenEdge[2]) * 16 / 255, sizeBase = static_cast<float>(b.screenEdge[3]) / 255;
    const bool atlas = b.atlasColumns > 1 || b.atlasRows > 1;
    const float columns = b.atlasColumns, rows = b.atlasRows;
    int columnBits = 0;
    while (columnBits < 8 && (b.atlasColumns >> (columnBits + 1)) != 0) {
        ++columnBits;
    }
    const auto& c = view.clip;

    for (const Particle& p : emitter.particles()) {
        const float t = p.age / p.life;
        const Vec3 at = plus(times(turned(p.position, parent.rotation), parent.scale), parent.position);

        float size = b.size.at(t) * emitter.liveSize() * parent.scale * p.size;
        float alpha = b.alpha.at(t) * emitter.alpha() * p.alpha;
        if (fades) {
            const float w = c[12] * at[0] + c[13] * at[1] + c[14] * at[2] + c[15];
            const float x = (c[0] * at[0] + c[1] * at[1] + c[2] * at[2] + c[3]) / w;
            const float y = (c[4] * at[0] + c[5] * at[1] + c[6] * at[2] + c[7]) / w;
            const float edge = 1 - std::max(std::abs(x), std::abs(y));
            alpha *= edge > 0 ? std::min(1.0f, alphaSlope * edge + alphaBase) : alphaBase;
            size *= edge > 0 ? std::min(1.0f, sizeSlope * edge + sizeBase) : sizeBase;
        }

        // A sprite that stretches lines its up along its flight on screen, and grows longer past the block's speed.
        Vec3 up = startUp, right = startRight;
        if (b.stretch != 0) {
            const float along = dot(p.velocity, up), across = dot(p.velocity, right);
            const float speed = std::hypot(along, across);
            if (speed > 1e-6f) {
                const float a = along / speed, s = across / speed;
                const Vec3 newUp = plus(times(right, s), times(up, a));
                right = minus(times(right, a), times(up, s));
                up = speed > b.stretch ? times(newUp, speed / b.stretch) : newUp;
            }
        }
        up = times(up, size);
        right = times(right, size);

        const float aspect = b.aspect.at(t) * p.aspect;
        const bool aspectAfter = (b.flags & Block::kAspectAfterTurn) != 0;
        if (!aspectAfter) {
            right = times(right, aspect);
        }
        const float turn = b.rotation.at(t) * p.rotation + b.rotationOffset;
        if (std::abs(turn) > 1e-6f) {
            const float s = std::sin(turn * kTwoPi), k = std::cos(turn * kTwoPi);
            const Vec3 newUp = minus(times(up, k), times(right, s));
            right = plus(times(up, s), times(right, k));
            up = newUp;
        }
        if (aspectAfter) {
            right = times(right, aspect);
        }

        const Vec3 pivot = sample(b.pivots, t, {0, 0, 0});
        const Vec3 corner = minus(minus(at, times(right, pivot[0] + p.pivot[0])), times(up, pivot[1] + p.pivot[1]));

        float u0 = 0, v0 = 0, du = 1, dv = 1;
        if (atlas) {
            auto frame = static_cast<std::uint32_t>(static_cast<std::int64_t>(b.framesPerSecond * p.age + static_cast<float>(p.firstFrame)));
            if (b.frameCycle != 0) {
                frame = (frame - emitter.firstFrame()) % b.frameCycle + emitter.firstFrame();
            }
            du = 1 / columns;
            dv = 1 / rows;
            u0 = static_cast<float>(frame & (b.atlasColumns - 1u)) * du;
            v0 = static_cast<float>((frame >> columnBits) & (b.atlasRows - 1u)) * dv;
        }

        const Vec3 tint = sample(b.colours, t, {1, 1, 1});
        const auto& live = emitter.colour();
        const std::array<std::uint8_t, 4> colour{byteOf(tint[0] * p.colour[0] * live[0]), byteOf(tint[1] * p.colour[1] * live[1]),
                                                 byteOf(tint[2] * p.colour[2] * live[2]), byteOf(alpha)};
        const ParticleVertex a{plus(corner, up), {u0, v0}, colour};
        const ParticleVertex d{corner, {u0, v0 + dv}, colour};
        const ParticleVertex e{plus(corner, right), {u0 + du, v0 + dv}, colour};
        const ParticleVertex g{plus(plus(corner, right), up), {u0 + du, v0}, colour};
        out.insert(out.end(), {a, d, e, a, e, g});
    }
}

} // namespace ts2::scene
