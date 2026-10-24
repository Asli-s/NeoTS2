#include "scene/neighborhood_plumbobs.hpp"

#include "diag/log.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::scene {

namespace {

constexpr float kTwoPi = 6.28318530717958647692f;
constexpr std::uint8_t kAdded = 4;  // the blend byte of additive sprites (stdPMAdditive)

// The highest draw layer a sprite may ask for, far above any the game's files use.
constexpr float kMaxLayer = 1000.0f;

bool finite(const effects::Curve& c) {
    return std::all_of(c.values.begin(), c.values.end(), [](float v) { return std::isfinite(v); });
}

bool finite(const std::array<float, 3>& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// Whether the bundle's particle system is one this plays: one particle at a time with a fixed life,
// starting at a fixed point. Values that aren't numbers (a broken file) leave it out.
bool playable(const effects::ParticleBlock& b) {
    if (!finite(b.size) || !finite(b.aspect) || !finite(b.rotation) || !finite(b.alpha) || !std::isfinite(b.prewarm) ||
        !std::isfinite(b.interval) || !std::isfinite(b.rotationScale[0]) || !std::isfinite(b.rotationOffset) ||
        !std::isfinite(b.life[0]) || !finite(b.spawnLow) || !finite(b.rotationAxis) ||
        !std::all_of(b.colours.begin(), b.colours.end(), [](const auto& c) { return finite(c); }) ||
        !(std::abs(b.layer) <= kMaxLayer)) {
        return false;
    }
    const bool oneAtATime = (b.flags & (effects::ParticleBlock::kBursts | effects::ParticleBlock::kHoldCount)) != 0 &&
                            std::lround(b.count.at(0)) == 1 && b.burstLimit == 0;
    return oneAtATime && b.life[0] > 0 && b.life[0] == b.life[1] && b.spawnLow == b.spawnHigh &&
           (!(b.flags & effects::ParticleBlock::kBursts) || b.interval > 0);
}

// How far through its life the particle is at `seconds`, or a negative value while none is alive.
// The system has already played its prewarm when first shown. Bursts start a particle every
// interval, a held count replaces it as soon as it dies.
float lifeShare(const effects::ParticleBlock& b, double seconds) {
    const double t = seconds + b.prewarm;
    const double age = (b.flags & effects::ParticleBlock::kBursts) ? std::fmod(t, static_cast<double>(b.interval))
                                                                    : std::fmod(t, static_cast<double>(b.life[0]));
    return age < b.life[0] ? static_cast<float>(age / b.life[0]) : -1.0f;
}

std::array<float, 3> cross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

// A square from -0.5 to 0.5, facing +z, its texture upright (v down from the top edge).
int quadMesh(SceneBuilder& builder, int material) {
    Primitive p;
    p.material = material;
    p.positions = {-0.5f, 0.5f, 0, -0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0.5f, 0.5f, 0};
    p.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    p.uvs = {0, 0, 0, 1, 1, 1, 1, 0};
    p.indices = {0, 1, 2, 0, 2, 3};
    Surfaces s;
    s.emplace(material, std::move(p));
    return builder.addMesh("particle sprite", std::move(s));
}

// A shape's mesh with its materials times the particle's colour and alpha, as the particle model
// material's colour scalar does (taken at the start of its life, the plumbob's never change).
int tinted(SceneBuilder& builder, int mesh, const effects::ParticleBlock& b) {
    auto& scene = builder.scene();
    if (mesh < 0 || static_cast<std::size_t>(mesh) >= scene.meshes.size()) {
        return -1;
    }
    const auto& colour = b.colours.empty() ? std::array<float, 3>{1, 1, 1} : b.colours.front();
    const float alpha = b.alpha.at(0);
    if (colour == std::array<float, 3>{1, 1, 1} && alpha == 1) {
        return mesh;
    }
    Mesh copy = scene.meshes[static_cast<std::size_t>(mesh)];
    for (auto& p : copy.primitives) {
        if (p.material < 0 || static_cast<std::size_t>(p.material) >= scene.materials.size()) {
            continue;
        }
        Material m = scene.materials[static_cast<std::size_t>(p.material)];
        for (int i = 0; i < 3; ++i) {
            m.color[static_cast<std::size_t>(i)] *= colour[static_cast<std::size_t>(i)];
        }
        m.color[3] *= alpha;
        if (alpha < 1 && m.alpha == AlphaMode::Opaque) {
            m.alpha = AlphaMode::Blend;
        }
        scene.materials.push_back(std::move(m));
        p.material = static_cast<int>(scene.materials.size() - 1);
    }
    scene.meshes.push_back(std::move(copy));
    return static_cast<int>(scene.meshes.size() - 1);
}

} // namespace

std::size_t NeighborhoodPlumbobs::add(SceneBuilder& builder, const effects::File& file, const std::string& effect,
                                      const std::vector<std::array<float, 3>>& anchors) {
    const effects::VisualEffect* top = file.find(effect);
    if (top == nullptr) {
        return 0;
    }
    // The effect's metaparticles each play a bundle where their particle is: at the start of their
    // spawn box, its systems scaled by their size.
    struct System {
        std::size_t block = 0;
        int mesh = -1;
        std::array<float, 3> offset{};
        float scale = 1;
    };
    std::vector<System> systems;
    for (const auto& c : top->components) {
        if (c.type != static_cast<std::uint8_t>(effects::BlockType::MetaParticles) || c.index < 0 ||
            static_cast<std::size_t>(c.index) >= file.metaParticles.size()) {
            continue;
        }
        const effects::MetaParticleBlock& meta = file.metaParticles[static_cast<std::size_t>(c.index)];
        const effects::VisualEffect* bundle = file.find(meta.effect);
        if (bundle == nullptr || meta.spawnLow != meta.spawnHigh || !finite(meta.spawnLow) || !finite(meta.size)) {
            continue;
        }
        for (const auto& part : bundle->components) {
            if (part.type != static_cast<std::uint8_t>(effects::BlockType::Particles) || part.index < 0 ||
                static_cast<std::size_t>(part.index) >= file.particles.size()) {
                continue;
            }
            const effects::ParticleBlock& b = file.particles[static_cast<std::size_t>(part.index)];
            if (!playable(b)) {
                diag::warnOnce("neighborhood plumbobs: a particle system of a kind not played yet is left out of " + meta.effect);
                continue;
            }
            int mesh = -1;
            if (b.flags & effects::ParticleBlock::kModel) {
                mesh = tinted(builder, builder.model(b.texture, {}), b);
            } else {
                const auto& colour = b.colours.empty() ? std::array<float, 3>{1, 1, 1} : b.colours.front();
                const int material = builder.particleMaterial(b.texture, {colour[0], colour[1], colour[2], b.alpha.at(0)}, b.blend == kAdded,
                                                              static_cast<int>(b.layer));
                mesh = material < 0 ? -1 : quadMesh(builder, material);
            }
            if (mesh < 0) {
                diag::warnOnce("neighborhood plumbobs: " + b.texture + " is missing");
                continue;
            }
            blocks_.push_back(b);
            systems.push_back({blocks_.size() - 1, mesh, meta.spawnLow, meta.size.at(0)});
        }
    }
    auto& instances = builder.scene().instances;
    for (const auto& anchor : anchors) {
        for (const auto& s : systems) {
            const auto& spawn = blocks_[s.block].spawnLow;
            const std::array<float, 3> at{anchor[0] + s.offset[0] + spawn[0] * s.scale, anchor[1] + s.offset[1] + spawn[1] * s.scale,
                                          anchor[2] + s.offset[2] + spawn[2] * s.scale};
            if (Instance* placed = builder.place(s.mesh, kNeighborhoodPlumbobName, Transform::at(at[0], at[1], at[2], 0)); placed != nullptr) {
                // Shapes are lit as outdoor objects (the scene's object lighting), sprites aren't lit.
                placed->litOutdoors = (blocks_[s.block].flags & effects::ParticleBlock::kModel) != 0;
                parts_.push_back({instances.size() - 1, s.block, at, s.scale});
            }
        }
    }
    return systems.size();
}

void NeighborhoodPlumbobs::place(double seconds, const std::array<float, 3>& right, const std::array<float, 3>& up,
                                 std::vector<std::pair<std::size_t, Transform>>& out) const {
    const std::array<float, 3> normal = cross(right, up);
    for (const auto& part : parts_) {
        const effects::ParticleBlock& b = blocks_[part.block];
        const float t = lifeShare(b, seconds);
        Transform place;
        if (t < 0) {
            place.m.fill(0.0f);  // no particle alive: nothing drawn
            out.emplace_back(part.instance, place);
            continue;
        }
        const float size = b.size.at(t) * part.scale;
        const float turn = b.rotation.at(t) * b.rotationScale[0] + b.rotationOffset;
        if (b.flags & effects::ParticleBlock::kModel) {
            // A shape turns about its rotation axis by radians, scaled by its size (kept within -1..1).
            const float scale = std::clamp(size, -1.0f, 1.0f);
            const auto& axis = b.rotationAxis;
            const float length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
            const float half = length > 0 ? turn / 2 : 0;
            const float k = length > 0 ? std::sin(half) / length : 0;
            place = Transform::of(part.at, {axis[0] * k, axis[1] * k, axis[2] * k, std::cos(half)});
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    place.m[static_cast<std::size_t>(row * 4 + column)] *= scale;
                }
            }
        } else {
            // A sprite faces the camera, `size` tall: its corners are turned (in full turns), then
            // its width is narrowed by the aspect along the camera's right.
            const float aspect = b.aspect.at(t);
            const float c = std::cos(turn * kTwoPi), s = std::sin(turn * kTwoPi);
            for (int row = 0; row < 3; ++row) {
                const auto r = static_cast<std::size_t>(row);
                place.m[r * 4 + 0] = size * (c * aspect * right[r] + s * up[r]);
                place.m[r * 4 + 1] = size * (-s * aspect * right[r] + c * up[r]);
                place.m[r * 4 + 2] = normal[r];
                place.m[r * 4 + 3] = part.at[r];
            }
        }
        out.emplace_back(part.instance, place);
    }
}

} // namespace ts2::scene
