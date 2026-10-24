#include "scene/hood_drift.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace ts2::scene {

namespace {

constexpr float kPi = 3.14159265358979f;

// The index of the first component of `effect` that is a block of `type`.
std::optional<std::size_t> part(const effects::VisualEffect* effect, effects::BlockType type, std::size_t blocks) {
    if (effect) {
        for (const auto& c : effect->components) {
            if (c.type == static_cast<std::uint8_t>(type) && c.index >= 0 && static_cast<std::size_t>(c.index) < blocks) {
                return static_cast<std::size_t>(c.index);
            }
        }
    }
    return std::nullopt;
}

// Finite and well inside what floats can add up over a long drift. The game's values are all
// below 10,000.
constexpr float kMaxValue = 1e6f;

bool usable(float v) { return std::isfinite(v) && std::abs(v) <= kMaxValue; }

bool usable(std::initializer_list<float> values) {
    return std::all_of(values.begin(), values.end(), [](float v) { return usable(v); });
}

} // namespace

std::optional<HoodDrift> HoodDrift::load(const effects::File& file, std::uint32_t seed) {
    using effects::BlockType;
    const auto sequence = part(file.find("nhoodcam"), BlockType::Sequence, file.sequences.size());
    if (!sequence) {
        return std::nullopt;
    }
    HoodDrift drift;
    drift.seed_ = seed == 0 ? 1 : seed;
    const effects::SequenceBlock::Item* motion = nullptr;
    for (const auto& item : file.sequences[*sequence].items) {
        if (!item.effect.empty()) {
            motion = &item;
            break;
        }
        drift.waitRanges_.push_back(item.range);
    }
    if (!motion) {
        return std::nullopt;
    }
    drift.playRange_ = motion->range;
    const auto particles = part(file.find(motion->effect), BlockType::MetaParticles, file.metaParticles.size());
    if (!particles) {
        return std::nullopt;
    }
    drift.particles_ = file.metaParticles[*particles];
    const auto camera = part(file.find(drift.particles_.effect), BlockType::Camera, file.cameras.size());
    if (!camera) {
        return std::nullopt;
    }
    drift.camera_ = file.cameras[*camera];
    if (!drift.sensible()) {
        return std::nullopt;
    }
    return drift;
}

// Every value the drift uses must be usable, in a range it can work with. A damping over 15 would
// reverse the particle's speed in a 1/15 s step, and the camera's goal would fly off.
bool HoodDrift::sensible() const {
    bool ok = usable({playRange_[0], playRange_[1], particles_.life[0], particles_.life[1]}) && particles_.damping >= 0 &&
              particles_.damping <= 15 && camera_.length > 0 && usable(camera_.length);
    for (const auto& range : waitRanges_) {
        ok = ok && usable({range[0], range[1]});
    }
    for (const auto& w : particles_.wander) {
        ok = ok && usable({w.retime[0], w.retime[1], w.speed[0], w.speed[1], w.turn, w.leastTurn});
    }
    for (float v : camera_.curves[5].values) {
        ok = ok && usable(v);
    }
    return ok;
}

void HoodDrift::interrupt() {
    rested_ = 0;
    running_ = false;
}

HoodDrift::Step HoodDrift::step(const std::array<float, 3>& target, float seconds) {
    Step out;
    if (!running_) {
        rested_ += seconds;
        if (rested_ > 30.5f / 120) {
            start(target);
        }
        if (!spawnPending_) {
            return out;
        }
    } else if (waiting_ > 0) {
        waiting_ -= seconds;
        if (waiting_ > 0) {
            return out;
        }
        spawn();
    } else {
        played_ += seconds;
        if (played_ >= playLength_ || age_ >= life_) {
            return out;
        }
        age_ += seconds;
        wander(seconds);
        // The camera block's own clock runs from 0 to 1 over its length and then holds.
        clock_ = std::min(clock_ + seconds / camera_.length, 1.0f);
    }
    // A new particle takes the last drift's sway into the yaw, so the view doesn't jump.
    out.foldSway = spawnPending_;
    spawnPending_ = false;
    if (camera_.flags & 0x40) {
        out.sway = camera_.curves[5].at(clock_) * kPi / 180;
    }
    if (camera_.flags & 0x800) {
        out.goal = std::array<float, 3>{base_[0] + position_[0], base_[1] + position_[1], base_[2] + position_[2]};
    }
    return out;
}

// The game's random numbers: a multiplying generator, a value in [0, 1) from the state read as a
// signed number, scaled into a range.
float HoodDrift::random(float low, float high) {
    seed_ *= 0x278DDE6Du;
    const double u = static_cast<double>(static_cast<std::int32_t>(seed_)) / 4294967296.0 + 0.5;
    return static_cast<float>(low + (high - low) * u);
}

void HoodDrift::start(const std::array<float, 3>& target) {
    running_ = true;
    base_ = target;
    waiting_ = 0;
    for (const auto& range : waitRanges_) {
        waiting_ += random(range[0], range[1]);
    }
    playLength_ = random(playRange_[0], playRange_[1]);
    played_ = 0;
    if (waiting_ <= 0) {
        spawn();
    }
}

// A new particle at the base, still, both pulls facing +x with fresh timers and strengths.
void HoodDrift::spawn() {
    waiting_ = 0;
    spawnPending_ = true;
    age_ = 0;
    clock_ = 0;
    life_ = random(particles_.life[0], particles_.life[1]);
    position_ = {0, 0, 0};
    velocity_ = {0, 0, 0};
    for (std::size_t k = 0; k < channels_.size(); ++k) {
        const auto& w = particles_.wander[k];
        channels_[k] = {random(w.retime[0], w.retime[1]), random(w.speed[0], w.speed[1]), 0};
    }
}

// The particle moves in steps of at most 1/15 s: it moves by its speed, its two pulls are
// re-picked when their timers run out and push it, and it loses `damping` of its speed a second.
void HoodDrift::wander(float seconds) {
    for (float left = seconds; left > 0;) {
        const float h = std::min(1.0f / 15, left);
        // Held within the value limit, so a drift without damping can't carry the goal off.
        for (std::size_t k = 0; k < 3; ++k) {
            position_[k] = std::clamp(position_[k] + velocity_[k] * h, -kMaxValue, kMaxValue);
        }
        std::array<float, 2> pull{0, 0};
        for (std::size_t k = 0; k < channels_.size(); ++k) {
            auto& c = channels_[k];
            const auto& w = particles_.wander[k];
            c.timer -= h;
            if (c.timer < 0) {
                c.timer += random(w.retime[0], w.retime[1]);
                c.speed = random(w.speed[0], w.speed[1]);
                const float x = random(-(w.turn + 1e-5f), w.turn + 1e-5f);
                c.yaw = std::remainder(c.yaw + 2 * kPi * x + (x >= 0 ? 2 * kPi * w.leastTurn : -2 * kPi * w.leastTurn), 2 * kPi);
            }
            pull[0] += c.speed * std::cos(c.yaw);
            pull[1] += c.speed * std::sin(c.yaw);
        }
        velocity_[0] += pull[0] * h;
        velocity_[1] += pull[1] * h;
        for (float& v : velocity_) {
            v *= 1 - h * particles_.damping;
        }
        left -= h;
    }
}

} // namespace ts2::scene
