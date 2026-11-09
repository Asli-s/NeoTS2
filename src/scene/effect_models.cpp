#include "scene/effect_models.hpp"

#include "diag/log.hpp"
#include "scene/assets.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace ts2::scene {

namespace {

// The most instances kept for one model of one effect, against files whose counts would fill the
// scene. The game's own neighborhood effects show a few of each.
constexpr std::size_t kMostPerModel = 64;
// The most instances of one model in the whole scene, against a neighborhood with countless
// decorations showing it.
constexpr std::size_t kMostOfOneModel = 1024;
constexpr int kMostNesting = 16;
constexpr std::string_view kModelSuffix = "_cres";

using Counts = std::map<std::string, std::size_t>;

void addCounts(Counts& into, const Counts& from, std::size_t times) {
    for (const auto& [model, n] : from) {
        std::size_t& total = into[model];
        total = std::min(total + std::min(n * times, kMostPerModel), kMostPerModel);
    }
}

float most(const effects::Curve& c) {
    float out = 0;
    for (const float v : c.values) {
        if (std::isfinite(v)) {
            out = std::max(out, v);
        }
    }
    return out;
}

// The most particles a metaparticle block keeps alive at once: its count when it holds a count,
// otherwise what its bursts or flow make over a particle's longest life.
std::size_t mostParticles(const effects::MetaParticleBlock& b) {
    using Block = effects::MetaParticleBlock;
    const float count = std::ceil(most(b.count));
    const float life = std::isfinite(b.life[1]) ? std::max({b.life[0], b.life[1], 0.0f}) : 0.0f;
    float n = count;
    if (b.flags & Block::kHoldCount) {
        n = count;
    } else if (b.flags & Block::kBursts) {
        const float interval = std::max(b.interval, 1e-3f);
        n = count * (std::ceil(life / interval) + 1);
        if (b.burstLimit > 0) {
            n = std::min(n, count * static_cast<float>(b.burstLimit));
        }
    } else {
        n = std::ceil(count * life) + 1;
    }
    return std::isfinite(n) ? static_cast<std::size_t>(std::clamp(n, 0.0f, static_cast<float>(kMostPerModel))) : kMostPerModel;
}

// How many of each model the effect can show at once, at most. Every part counts, whatever the
// state, level of detail or group picks.
Counts modelCounts(const effects::File& file, std::string_view name, int nesting) {
    Counts out;
    const effects::VisualEffect* effect = file.find(name);
    if (effect == nullptr || nesting >= kMostNesting) {
        return out;
    }
    for (const auto& c : effect->components) {
        const auto index = static_cast<std::size_t>(c.index);
        if (c.type == 0) {
            addCounts(out, modelCounts(file, c.name, nesting + 1), 1);
        } else if (c.type == static_cast<std::uint8_t>(effects::BlockType::Model) && c.index >= 0 && index < file.models.size()) {
            addCounts(out, {{file.models[index].model, 1}}, 1);
        } else if (c.type == static_cast<std::uint8_t>(effects::BlockType::MetaParticles) && c.index >= 0 && index < file.metaParticles.size()) {
            const auto& meta = file.metaParticles[index];
            addCounts(out, modelCounts(file, meta.effect, nesting + 1), mostParticles(meta));
        } else if (c.type == static_cast<std::uint8_t>(effects::BlockType::Sequence) && c.index >= 0 && index < file.sequences.size()) {
            // A replaced item can finish while the next plays, so every item counts.
            for (const auto& item : file.sequences[index].items) {
                if (!item.effect.empty()) {
                    addCounts(out, modelCounts(file, item.effect, nesting + 1), 1);
                }
            }
        }
    }
    return out;
}

} // namespace

std::size_t EffectModels::reserve(SceneBuilder& builder, const effects::File& file, const std::string& effect) {
    std::size_t added = 0;
    for (const auto& [model, count] : modelCounts(file, effect, 0)) {
        std::string name = model;
        if (name.size() > kModelSuffix.size() && lower(name.substr(name.size() - kModelSuffix.size())) == kModelSuffix) {
            name.resize(name.size() - kModelSuffix.size());
        }
        const int mesh = builder.model(name, {});
        if (mesh < 0) {
            diag::warnOnce("effects: model " + model + " is missing");
            continue;
        }
        auto& kept = instances_[lower(model)];
        if (kept.size() + count > kMostOfOneModel) {
            diag::warnOnce("effects: model " + model + " has more instances than the scene keeps");
        }
        for (std::size_t i = 0; i < count && kept.size() < kMostOfOneModel; ++i) {
            if (Instance* placed = builder.place(mesh, "effect " + name, Transform{})) {
                // Lit by the same lights as the neighborhood's trees, so they darken at night too.
                placed->litOutdoors = true;
                placed->seaReflected = true;
                kept.push_back(builder.scene().instances.size() - 1);
                ++added;
            }
        }
    }
    return added;
}

std::array<float, 4> modelTurn(const EmitterPlace& place) {
    const auto& m = place.rotation;
    std::array<float, 4> q{};
    const float trace = m[0] + m[4] + m[8];
    if (trace > 0) {
        const float r = std::sqrt(trace + 1);
        const float f = 0.5f / r;
        q = {(m[7] - m[5]) * f, (m[2] - m[6]) * f, (m[3] - m[1]) * f, 0.5f * r};
    } else {
        // Built from the largest of the diagonal.
        std::size_t i = m[0] < m[4] ? 1 : 0;
        if (m[i * 4] < m[8]) {
            i = 2;
        }
        const std::size_t j = (i + 1) % 3, k = (j + 1) % 3;
        const float r = std::sqrt(m[i * 4] - (m[k * 4] + m[j * 4]) + 1);
        const float f = 0.5f / r;
        q[i] = 0.5f * r;
        q[3] = (m[k * 3 + j] - m[j * 3 + k]) * f;
        q[j] = (m[i * 3 + j] + m[j * 3 + i]) * f;
        q[k] = (m[i * 3 + k] + m[k * 3 + i]) * f;
    }
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(length > 0) || !std::isfinite(length)) {
        return {0, 0, 0, 1};
    }
    for (float& v : q) {
        v /= length;
    }
    return q;
}

void EffectModels::place(const EffectPlayer* player, std::vector<Placed>& out) const {
    out.clear();
    std::map<std::string, std::size_t> used;
    if (player != nullptr) {
        player->forEachModel([&](const effects::ModelBlock& block, const EmitterPlace& where, const std::array<float, 4>& colour) {
        const auto kept = instances_.find(lower(block.model));
        if (kept == instances_.end()) {
            return;
        }
        std::size_t& n = used[kept->first];
        if (n >= kept->second.size()) {
            diag::warnOnce("effects: model " + block.model + " is shown more often than it has instances");
            return;
        }
        if (!std::all_of(where.position.begin(), where.position.end(), [](float v) { return std::isfinite(v); })) {
            return;
        }
        Placed p;
        p.instance = kept->second[n++];
        p.shown = true;
        p.transform = Transform::of(where.position, modelTurn(where));
        // Drawn as it is when all four are 1 to within 32 steps of a float.
        if (std::any_of(colour.begin(), colour.end(), [](float v) { return !(std::abs(v - 1) <= 32 * FLT_EPSILON); })) {
            p.shapeColor = colour;
        }
        out.push_back(p);
    });
    }
    for (const auto& [model, instances] : instances_) {
        const auto u = used.find(model);
        for (std::size_t i = u == used.end() ? 0 : u->second; i < instances.size(); ++i) {
            Placed p;
            p.instance = instances[i];
            out.push_back(p);
        }
    }
}

} // namespace ts2::scene
