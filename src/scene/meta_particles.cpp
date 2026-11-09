#include "scene/meta_particles.hpp"

#include "diag/log.hpp"
#include "scene/effect_player.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ts2::scene {

namespace {

using Block = effects::MetaParticleBlock;
using Vec3 = std::array<float, 3>;

constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kHalfPi = 1.57079632679489661923f;

constexpr float kLongestStep = 1.0f / 15.0f;
constexpr float kShortestCycle = 1e-6f;
constexpr float kOneStepDrag = 0.05f;  // a particle moves in one step unless its drag over that step passes this

// Blocks with these flags need small steps.
constexpr std::uint64_t kSmallStepFlags = 0x29000360;

// How particles meet the terrain. The block's terrain values are named by their place in its list.
constexpr std::uint64_t kStayAboveGround = 0x40;  // a particle below the ground comes back up to it
constexpr std::uint64_t kOnGround = 0x80;         // with kStayAboveGround: it always lies on the ground
constexpr std::uint64_t kAvoidShallows = 0x100;   // it steers away from ground ahead
constexpr std::uint64_t kStayInside = 0x200;      // it dies at the terrain's edge
constexpr std::uint64_t kPinned = 0x400;          // it lies on the ground
constexpr std::uint64_t kStartOnGround = 0x4000;
constexpr std::uint64_t kStartOnWater = 0x8000;   // with kStartOnGround: on whichever is higher
constexpr std::uint64_t kStartAtDepth = 0x10000;  // only where the ground lies between two depths below the water
constexpr std::uint64_t kStartsOnTerrain = 0x7C000;
constexpr std::uint64_t kTerrainFlags = kStayAboveGround | kOnGround | kAvoidShallows | kStayInside | kPinned | kStartsOnTerrain;
constexpr std::size_t kBounce = 0;       // not 0: a particle back up from the ground bounces off it by this much
constexpr std::size_t kAvoidHeight = 1;  // ground ahead closer below than this pushes it away
constexpr std::size_t kAvoidStrength = 2;
constexpr std::size_t kLookAhead = 3;    // how far ahead it looks, in seconds of its flight
constexpr std::size_t kAvoidUpward = 4;  // the share of the push that goes up
constexpr std::size_t kDeathHeight = 5;  // ground ahead closer below than this kills it
constexpr std::size_t kBounceDeath = 6;  // the chance a bounce kills it
constexpr float kLeastPull = 0.001f;   // a weaker pull isn't steered
constexpr std::size_t kLowestDepth = 8, kHighestDepth = 9;  // with kStartAtDepth, the ground's height minus the water's

// Flags for behaviour not played yet: spawn balls (0x800), box sizes from the terrain (0x1000, 0x2000), a head
// start along the flight (0x10) and the small step flags other than wandering and the terrain's. A block that
// follows a path takes no small steps.
constexpr std::uint64_t kNotPlayedFlags =
    (kSmallStepFlags & ~(Block::kWanderEach | kStayAboveGround | kAvoidShallows | kStayInside)) | 0x800 | 0x1000 | 0x2000 | 0x10;

// The effect script's align values for a particle's effect turned along its flight, by the axis that follows it.
constexpr std::uint8_t kAlignX = 2, kAlignY = 3, kAlignZ = 4;
constexpr float kAlignEase = 1.0f / 15.0f;  // times 1 / alignSmoothing, the share of the way eased each update

// Every particle starts an effect, so a broken file must not ask for too many. The game's files keep far fewer.
constexpr std::size_t kMostParticles = 2000;
constexpr int kMostStepsAnUpdate = 500000;

template <std::size_t N>
bool finite(const std::array<float, N>& values) {
    return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
}

bool finite(const effects::Curve& curve) {
    return std::all_of(curve.values.begin(), curve.values.end(), [](float v) { return std::isfinite(v); });
}

// False for a block with values that aren't numbers, or times too long to play.
bool playable(const Block& b) {
    constexpr float kLongest = 1e6f;
    return finite(b.life) && finite(std::array<float, 1>{b.prewarm}) && finite(b.delay) && finite(b.gap) && finite(b.directionLow) && finite(b.directionHigh) &&
           finite(b.speed) && finite(b.spawnLow) && finite(b.spawnHigh) && finite(b.count) && finite(std::array<float, 1>{b.interval}) && finite(b.gravity) &&
           finite(std::array<float, 3>{b.windShare, b.blast, b.damping}) && finite(std::array<float, 3>{b.sizeVary, b.rotationVary, b.alphaVary}) && finite(b.colourVary) &&
           std::abs(b.prewarm) <= kLongest && std::abs(b.life[0]) <= kLongest && std::abs(b.life[1]) <= kLongest &&
           std::all_of(b.path.begin(), b.path.end(), [](const Block::PathPoint& p) { return finite(p.position) && finite(p.velocity) && std::isfinite(p.time); }) &&
           std::all_of(b.waves.begin(), b.waves.end(), [](const Block::Wave& w) { return std::isfinite(w.frequency) && finite(w.phase) && finite(w.amplitude); }) &&
           finite(b.terrain) && std::isfinite(b.alignSmoothing);
}

float lengthSquared(const Vec3& v) { return v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; }

Vec3 turned(const Vec3& v, const std::array<float, 9>& r) {
    return {r[0] * v[0] + r[1] * v[1] + r[2] * v[2], r[3] * v[0] + r[4] * v[1] + r[5] * v[2], r[6] * v[0] + r[7] * v[1] + r[8] * v[2]};
}

// How many whole particles a count asks for: any part of one makes a whole one.
std::int64_t wholeCount(float count) { return static_cast<std::int64_t>(std::ceil(std::clamp(count, 0.0f, static_cast<float>(kMostParticles)))); }

// The rotation `r` turned further about its own vertical.
void turnAboutVertical(std::array<float, 9>& r, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    for (std::size_t row = 0; row < 3; ++row) {
        const float x = r[row * 3], y = r[row * 3 + 1];
        r[row * 3] = x * c + y * s;
        r[row * 3 + 1] = y * c - x * s;
    }
}

// The list's value at `t`, blended between its evenly spaced entries. White for an empty list.
Vec3 colourAt(const std::vector<Vec3>& list, float t) {
    if (list.empty()) {
        return {1, 1, 1};
    }
    const float x = static_cast<float>(list.size() - 1) * std::clamp(t, 0.0f, 1.0f);
    const auto i = static_cast<std::size_t>(x);
    const float part = x - static_cast<float>(i);
    if (part <= 0 || i + 1 >= list.size()) {
        return list[std::min(i, list.size() - 1)];
    }
    Vec3 out{};
    for (std::size_t k = 0; k < 3; ++k) {
        out[k] = list[i][k] + (list[i + 1][k] - list[i][k]) * part;
    }
    return out;
}

} // namespace

MetaParticleEmitter::MetaParticleEmitter(const Block& block, EffectPlayer& player, std::uint32_t seed, int nesting)
    : block_(block), player_(player), nesting_(nesting), playable_(playable(block) && player.knows(block.effect)), randomState_(seed == 0 ? 1 : seed) {
    clockRate_ = 1.0 / std::max(block.interval, kShortestCycle);
    const std::uint64_t notPlayed = (block.flags & Block::kFollowsPath) ? kNotPlayedFlags & ~kSmallStepFlags : kNotPlayedFlags;
    if (block.flags & notPlayed || block.screw != 0 || block.alignment == kAlignX || block.alignment == kAlignZ || block.alignSpin != 0 ||
        block.screenEdge[0] != 0 || block.screenEdge[2] != 0) {
        diag::warnOnce("effects: metaparticles use motion not played yet (" + block.effect + ")");
    }
}

float MetaParticleEmitter::random(float low, float high) {
    randomState_ ^= randomState_ << 13;
    randomState_ ^= randomState_ >> 17;
    randomState_ ^= randomState_ << 5;
    const float share = static_cast<float>(randomState_ - 1) / 4294967296.0f;
    return low + (high - low) * share;
}

void MetaParticleEmitter::start(bool prewarm) {
    if (!playable_ || (prewarm && block_.burstLimit != 0 && !(block_.flags & Block::kSustain))) {
        return;
    }
    clock_ = 0;
    cycle_ = 0;
    owed_ = 0;
    stopping_ = false;
    running_ = true;
    hardStopped_ = false;
    acceleration_ = block_.gravity;
    if (block_.flags & Block::kWanderAll) {
        const Vec3 direction{random(block_.directionLow[0], block_.directionHigh[0]), random(block_.directionLow[1], block_.directionHigh[1]),
                             random(block_.directionLow[2], block_.directionHigh[2])};
        for (std::size_t k = 0; k < sharedPulls_.size(); ++k) {
            startPull(sharedPulls_[k], block_.wander[k], direction);
        }
    }
    if (block_.delay[0] >= 0) {
        wait_ = random(block_.delay[0], block_.delay[1]);
    } else if (block_.gap[0] >= 0) {
        wait_ = random(block_.gap[0], block_.gap[1]);
    } else {
        wait_ = 0;
    }
    prewarmNext_ = prewarm;
}

void MetaParticleEmitter::stop(bool hard) {
    if (!running_) {
        return;
    }
    hardStopped_ = hard;
    stopping_ = !hard;
    if (hard) {
        running_ = false;
        for (Particle& p : particles_) {
            finish(p);
        }
        particles_.clear();
        for (const auto& effect : dying_) {
            if (effect->running()) {
                effect->stop(true);
            }
        }
        dying_.clear();
    }
}

void MetaParticleEmitter::place(const EmitterPlace& own, const EmitterPlace& parent) {
    own_ = own;
    parent_ = parent;
}

void MetaParticleEmitter::setDetailScales(float rate, float size) {
    rateByDetail_ = rate;
    sizeByDetail_ = size;
}

void MetaParticleEmitter::setHidden(bool hidden) {
    hidden_ = hidden;
    for (const Particle& p : particles_) {
        if (p.effect) {
            p.effect->setHidden(hidden);
        }
    }
    for (const auto& effect : dying_) {
        effect->setHidden(hidden);
    }
}

// Picks a pull's next wait and strength, and turns its headings by a random amount of at least the least turn.
// A pull straight up has no yaw to turn, and a flat one no pitch.
void MetaParticleEmitter::choosePull(Pull& pull, const Block::Wander& w) {
    pull.timer += random(w.retime[0], w.retime[1]);
    const float strength = random(w.speed[0], w.speed[1]);
    if (w.upward >= 0.5f) {
        pull.flatStrength = 2 * (1 - w.upward) * strength;
        pull.upwardStrength = strength;
    } else {
        pull.flatStrength = strength;
        pull.upwardStrength = 2 * strength * w.upward;
    }
    const float reach = w.turn + 1e-5f;
    if (w.upward < 1) {
        const float x = random(-reach, reach);
        pull.yaw += kTwoPi * x + (x >= 0 ? kTwoPi * w.leastTurn : -kTwoPi * w.leastTurn);
    }
    if (w.upward > 0) {
        const float x = random(-reach, reach);
        pull.pitch += kTwoPi * x + (x >= 0 ? kTwoPi * w.leastTurn : -kTwoPi * w.leastTurn);
    }
}

// A pull that first heads along `direction`.
void MetaParticleEmitter::startPull(Pull& pull, const Block::Wander& w, const Vec3& direction) {
    const float flat = direction[0] * direction[0] + direction[1] * direction[1];
    if (flat >= 1e-6f) {
        pull.yaw = std::atan2(direction[1], direction[0]);
        pull.pitch = std::asin(direction[2] / std::sqrt(flat + direction[2] * direction[2]));
    } else {
        pull.yaw = 0;
        pull.pitch = direction[2] < 0 ? -kHalfPi : kHalfPi;
    }
    if (block_.flags & Block::kWanderAtOnce) {
        pull.timer = random(w.retime[0], w.retime[1]);
        const float strength = random(w.speed[0], w.speed[1]);
        if (w.upward < 0.5f) {
            pull.flatStrength = strength;
            pull.upwardStrength = 2 * strength * w.upward;
        } else {
            pull.flatStrength = 2 * (1 - w.upward) * strength;
            pull.upwardStrength = strength;
        }
    } else {
        pull.timer = 0;
        choosePull(pull, w);
    }
}

namespace {

// Adds a pull's force.
template <typename Pull>
void addForce(const Pull& pull, Vec3& force) {
    if (pull.upwardStrength == 0) {
        force[0] += std::cos(pull.yaw) * pull.flatStrength;
        force[1] += std::sin(pull.yaw) * pull.flatStrength;
    } else if (pull.flatStrength == 0) {
        force[2] += std::sin(pull.pitch) * pull.upwardStrength;
    } else {
        const float c = std::cos(pull.pitch);
        force[0] += std::cos(pull.yaw) * c * pull.flatStrength;
        force[1] += std::sin(pull.yaw) * c * pull.flatStrength;
        force[2] += std::sin(pull.pitch) * pull.upwardStrength;
    }
}

} // namespace

// A point in the spawn box, or the torus that fills it, picked by rejection in -1..1.
Vec3 MetaParticleEmitter::randomSpawnPoint() {
    const Vec3& low = block_.spawnLow;
    const Vec3& high = block_.spawnHigh;
    if (block_.ring < 0) {
        return {random(low[0], high[0]), random(low[1], high[1]), random(low[2], high[2])};
    }
    Vec3 unit{};
    for (int tries = 0; tries < 1000; ++tries) {
        unit = {random(-1, 1), random(-1, 1), random(-1, 1)};
        const float flat = unit[0] * unit[0] + unit[1] * unit[1];
        const float off = std::sqrt(flat) - 0.5f;
        if (0.25f - off * off >= unit[2] * unit[2] && flat > 0) {
            const float k = (1 - block_.ring) / std::sqrt(flat) + block_.ring;
            unit = {unit[0] * k, unit[1] * k, unit[2] * block_.ring};
            break;
        }
    }
    Vec3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        out[i] = (high[i] + low[i] + (high[i] - low[i]) * unit[i]) * 0.5f;
    }
    return out;
}

Vec3 MetaParticleEmitter::toWorld(const Vec3& at) const {
    const Vec3 turnedAt = turnToWorld(at);
    return {turnedAt[0] + parent_.position[0], turnedAt[1] + parent_.position[1], turnedAt[2] + parent_.position[2]};
}

Vec3 MetaParticleEmitter::fromWorld(const Vec3& at) const {
    return turnFromWorld({at[0] - parent_.position[0], at[1] - parent_.position[1], at[2] - parent_.position[2]});
}

Vec3 MetaParticleEmitter::turnToWorld(const Vec3& direction) const {
    const Vec3 out = turned(direction, parent_.rotation);
    return {out[0] * parent_.scale, out[1] * parent_.scale, out[2] * parent_.scale};
}

Vec3 MetaParticleEmitter::turnFromWorld(const Vec3& direction) const {
    const auto& r = parent_.rotation;
    const float k = parent_.scale != 0 ? 1 / parent_.scale : 0;
    const Vec3 out{r[0] * direction[0] + r[3] * direction[1] + r[6] * direction[2], r[1] * direction[0] + r[4] * direction[1] + r[7] * direction[2],
                   r[2] * direction[0] + r[5] * direction[1] + r[8] * direction[2]};
    return {out[0] * k, out[1] * k, out[2] * k};
}

// Where a new particle starts by the terrain: false where it may not start (off the terrain, or at the wrong
// depth). On the ground or the water, it is raised by a random height from the spawn box.
bool MetaParticleEmitter::onTerrain(Particle& p) {
    const Block& b = block_;
    Vec3 w = toWorld(p.position);
    if (!(w[0] >= 0 && w[1] >= 0 && w[0] < terrain_->width && w[1] < terrain_->depth)) {
        return false;
    }
    const float ground = terrain_->height(w[0], w[1]);
    const float depth = ground - terrain_->waterLevel;
    if ((b.flags & kStartAtDepth) && !(depth >= b.terrain[kLowestDepth] && depth <= b.terrain[kHighestDepth])) {
        return false;
    }
    if (b.flags & (kStartOnGround | kStartOnWater)) {
        if (b.flags & kStartOnGround) {
            w[2] = (b.flags & kStartOnWater) ? std::max(ground, terrain_->waterLevel) : ground;
        } else {
            w[2] = terrain_->waterLevel;
        }
        w[2] += random(b.spawnLow[2], b.spawnHigh[2]);
        p.position = fromWorld(w);
    }
    return true;
}

// Makes a particle that started `agedBy` seconds ago, and starts its effect. False when none was made. A
// particle the terrain doesn't let start counts as made.
bool MetaParticleEmitter::spawn(float agedBy) {
    if (particles_.size() >= kMostParticles) {
        diag::warnOnce("effects: metaparticles reached the most particles one may have (" + block_.effect + ")");
        return false;
    }
    const Block& b = block_;
    Particle p;
    p.life = random(b.life[0], b.life[1]);
    p.position = randomSpawnPoint();
    const Vec3 direction{random(b.directionLow[0], b.directionHigh[0]), random(b.directionLow[1], b.directionHigh[1]), random(b.directionLow[2], b.directionHigh[2])};
    const float speed = random(b.speed[0], b.speed[1]);
    const float squared = lengthSquared(direction);
    if (squared > 1.4e-20f) {
        const float k = speed / std::sqrt(squared);
        p.velocity = {direction[0] * k, direction[1] * k, direction[2] * k};
    } else {
        p.velocity = {speed, 0, 0};
    }
    if (b.blast > 0) {
        const Vec3 away{p.position[0] - b.blastCentre[0], p.position[1] - b.blastCentre[1], p.position[2] - b.blastCentre[2]};
        const float distance = lengthSquared(away);
        if (distance > 1e-6f) {
            const float k = b.blast / std::sqrt(distance);
            for (std::size_t i = 0; i < 3; ++i) {
                p.velocity[i] += away[i] * k;
            }
        }
    }
    p.position = turned(p.position, own_.rotation);
    for (std::size_t i = 0; i < 3; ++i) {
        p.position[i] = p.position[i] * own_.scale + own_.position[i];
    }
    p.velocity = turned(p.velocity, own_.rotation);
    if ((b.flags & kStartsOnTerrain) && terrain_ && !onTerrain(p)) {
        return true;
    }

    p.size = random(1 - b.sizeVary, 1 + b.sizeVary) * sizeByDetail_;
    if (b.flags & Block::kScaleParticles) {
        p.size *= own_.scale;
    }
    p.alpha = random(1 - b.alphaVary, 1 + b.alphaVary);
    for (std::size_t i = 0; i < 3; ++i) {
        p.colour[i] = random(1 - b.colourVary[i], 1 + b.colourVary[i]);
    }
    p.rotation = b.rotationVary > 0 ? random(-b.rotationVary, b.rotationVary) : 1;
    const float heading = std::atan2(own_.rotation[3], own_.rotation[0]) / kTwoPi;
    if (b.alignment == 1) {
        p.rotation += heading;
    }
    p.heading = heading;

    p.effect = player_.createInside(b.effect, nesting_ + 1);
    if (p.effect) {
        p.effect->setHidden(hidden_);
        if (!picture_.empty()) {
            p.effect->setPicture(picture_);
        }
        if (firstFrame_ >= 0) {
            p.effect->setFirstFrame(firstFrame_);
        }
        EmitterPlace at;
        at.position = p.position;
        p.effect->placeIn(at, parent_);
        p.effect->start(false);
    }
    if (b.flags & Block::kWanderEach) {
        for (std::size_t k = 0; k < p.pulls.size(); ++k) {
            startPull(p.pulls[k], b.wander[k], direction);
        }
    }
    if (agedBy > 0 && !step(p, agedBy)) {
        finish(p);
        return true;
    }
    particles_.push_back(std::move(p));
    return true;
}

// Ages a particle and moves it. False when it died of age.
bool MetaParticleEmitter::step(Particle& p, float seconds) {
    p.age += seconds;
    if (p.age >= p.life) {
        p.age = p.life;
        return false;
    }
    if (block_.flags & Block::kFollowsPath) {
        return followPath(p);
    }
    const float drag = block_.damping;
    if (!(block_.flags & kSmallStepFlags) && seconds * drag <= kOneStepDrag) {
        const Vec3 before = p.velocity;
        const float keep = 1 - seconds * drag;
        for (std::size_t i = 0; i < 3; ++i) {
            p.velocity[i] = (p.velocity[i] + acceleration_[i] * seconds) * keep;
            p.position[i] += (p.velocity[i] + before[i]) * seconds * 0.5f;
        }
        return true;
    }
    while (seconds > 0) {
        // Out of steps, what time is left goes in one.
        const float h = --budget_ > 0 ? std::min(seconds, kLongestStep) : seconds;
        for (std::size_t i = 0; i < 3; ++i) {
            p.position[i] += p.velocity[i] * h;
            p.velocity[i] += acceleration_[i] * h;
        }
        if (block_.flags & Block::kWanderEach) {
            Vec3 force{};
            for (std::size_t k = 0; k < p.pulls.size(); ++k) {
                p.pulls[k].timer -= h;
                if (p.pulls[k].timer < 0) {
                    choosePull(p.pulls[k], block_.wander[k]);
                }
                addForce(p.pulls[k], force);
            }
            for (std::size_t i = 0; i < 3; ++i) {
                p.velocity[i] += force[i] * h;
            }
        }
        const float keep = 1 - h * drag;
        for (float& v : p.velocity) {
            v *= keep;
        }
        if (terrain_ && (block_.flags & kTerrainFlags) && !meetTerrain(p, h)) {
            return false;
        }
        seconds -= h;
    }
    return true;
}

// One small step's meeting with the terrain. False when the particle dies of it.
bool MetaParticleEmitter::meetTerrain(Particle& p, float seconds) {
    const Block& b = block_;
    Vec3 w = toWorld(p.position);
    if ((b.flags & kStayInside) && !(w[0] > 0 && w[1] > 0 && w[0] < terrain_->width && w[1] < terrain_->depth)) {
        return false;
    }
    if (b.flags & kAvoidShallows) {
        // Looks along its flight, a fixed share of its speed when it is slow.
        const float speed = std::sqrt(lengthSquared(p.velocity));
        const float k = b.terrain[kLookAhead] * (speed > 10 ? 1 / speed : 0.1f);
        const Vec3 ahead = toWorld({p.position[0] + p.velocity[0] * k, p.position[1] + p.velocity[1] * k, p.position[2] + p.velocity[2] * k});
        // Off the terrain there is nothing ahead to steer from.
        const bool inside = ahead[0] > 0 && ahead[1] > 0 && ahead[0] < terrain_->width && ahead[1] < terrain_->depth;
        const float clear = inside ? ahead[2] - terrain_->height(ahead[0], ahead[1]) : 0;
        if (inside && clear < b.terrain[kDeathHeight]) {
            return false;
        }
        if (inside && clear < b.terrain[kAvoidHeight]) {
            const Vec3 n = terrain_->normal(ahead[0], ahead[1]);
            const float push = (b.terrain[kAvoidHeight] - clear) * (b.terrain[kAvoidHeight] - clear) * seconds * b.terrain[kAvoidStrength];
            const Vec3 force = turnFromWorld({n[0] * push, n[1] * push, n[2] * b.terrain[kAvoidUpward] * push});
            if (b.flags & Block::kWanderEach) {
                // It turns its first pull's heading instead of being pushed.
                Pull& pull = p.pulls[0];
                if (pull.flatStrength > kLeastPull) {
                    pull.yaw = std::atan2(std::sin(pull.yaw) + force[1] / pull.flatStrength, std::cos(pull.yaw) + force[0] / pull.flatStrength);
                }
                if (pull.upwardStrength > kLeastPull) {
                    diag::warnOnce("effects: metaparticles steering their climb from the ground aren't played yet (" + b.effect + ")");
                }
            } else {
                for (std::size_t i = 0; i < 3; ++i) {
                    p.velocity[i] += force[i];
                }
            }
        }
    }
    if (b.flags & (kStayAboveGround | kPinned)) {
        const float ground = terrain_->height(w[0], w[1]);
        const bool comesUp = (b.flags & kStayAboveGround) && (w[2] < ground || (b.flags & kOnGround));
        if (comesUp || (b.flags & kPinned)) {
            w[2] = ground;
            p.position = fromWorld(w);
        }
        if (comesUp) {
            const Vec3 n = turnFromWorld(terrain_->normal(w[0], w[1]));
            const float into = p.velocity[0] * n[0] + p.velocity[1] * n[1] + p.velocity[2] * n[2];
            if (b.terrain[kBounce] != 0) {
                for (std::size_t i = 0; i < 3; ++i) {
                    p.velocity[i] -= n[i] * (1 + b.terrain[kBounce]) * into;
                }
                if (b.terrain[kBounceDeath] > 0 && random(0, 1) < b.terrain[kBounceDeath]) {
                    return false;
                }
            } else if (into < 0) {
                for (std::size_t i = 0; i < 3; ++i) {
                    p.velocity[i] -= n[i] * into;
                }
            }
        }
    }
    return true;
}

// Puts a particle on the block's path for its age, on the smooth curve between the two points around it, which meets
// each point's position and velocity. The first stretch starts in the middle of the spawn box, moving along the
// middle of the direction box at the lowest speed. The path moves with the effect. False when the age is past the
// path's end.
bool MetaParticleEmitter::followPath(Particle& p) const {
    const auto& path = block_.path;
    if (path.empty()) {
        return true;
    }
    std::size_t next = 0;
    while (next + 1 < path.size() && path[next].time <= p.age) {
        ++next;
    }
    const Block::PathPoint& to = path[next];
    float end = to.time;
    if (end == 0) {
        end = p.life;
    } else if (end < p.age) {
        return false;
    }
    Block::PathPoint from;
    if (next > 0) {
        from = path[next - 1];
    } else {
        for (std::size_t i = 0; i < 3; ++i) {
            from.position[i] = (block_.spawnLow[i] + block_.spawnHigh[i]) * 0.5f;
            from.velocity[i] = (block_.directionLow[i] + block_.directionHigh[i]) * 0.5f * block_.speed[0];
        }
    }
    const float span = end - from.time;
    if (!(span > 1e-6f)) {
        return true;
    }
    const float s = (p.age - from.time) / span;
    const float s2 = s * s, s3 = s2 * s;
    const float toShare = 3 * s2 - 2 * s3;
    const float fromLean = (s3 - 2 * s2 + s) * span, toLean = (s3 - s2) * span;
    const float fromLeanRate = 3 * s2 - 4 * s + 1, toLeanRate = 3 * s2 - 2 * s;
    const float shareRate = (6 * s - 6 * s2) / span;
    Vec3 position{}, velocity{};
    for (std::size_t i = 0; i < 3; ++i) {
        position[i] = from.position[i] * (1 - toShare) + to.position[i] * toShare + from.velocity[i] * fromLean + to.velocity[i] * toLean;
        velocity[i] = from.velocity[i] * fromLeanRate + to.velocity[i] * toLeanRate + (to.position[i] - from.position[i]) * shareRate;
    }
    position = turned(position, own_.rotation);
    velocity = turned(velocity, own_.rotation);
    for (std::size_t i = 0; i < 3; ++i) {
        p.position[i] = position[i] * own_.scale + own_.position[i];
        p.velocity[i] = velocity[i] * own_.scale;
    }
    return true;
}

// Ends a dead particle's effect. The effect of a particle that died in a soft stop is watched until it ends, and the
// block's effect with "_death" after its name plays where it died.
void MetaParticleEmitter::finish(Particle& p) {
    if (p.effect) {
        p.effect->stop(hardStopped_);
        if (!hardStopped_ && p.effect->running()) {
            dying_.push_back(p.effect);
        }
        p.effect.reset();
    }
    if (hardStopped_) {
        return;
    }
    const std::string deathName = block_.effect + "_death";
    if (player_.knows(deathName)) {
        if (const auto effect = player_.createInside(deathName, nesting_ + 1)) {
            effect->setHidden(hidden_);
            EmitterPlace at;
            at.position = p.position;
            effect->placeIn(at, parent_);
            effect->start(false);
            dying_.push_back(effect);
        }
    }
}

// Keeps the count alive, replacing the dead, with no aging and no say for the rate.
void MetaParticleEmitter::hold(float seconds) {
    clock_ += seconds * clockRate_;
    if (clock_ < cycle_) {
        return;
    }
    for (std::int64_t missing = wholeCount(block_.count.at(0)) - static_cast<std::int64_t>(particles_.size()); missing > 0; --missing) {
        if (!spawn(0)) {
            return;
        }
    }
}

// Makes the count at the start of every cycle, each one as old as the cycle is late.
void MetaParticleEmitter::burst(float seconds) {
    clock_ += seconds * clockRate_;
    if (clock_ < cycle_) {
        return;
    }
    const std::int64_t count = wholeCount(block_.count.at(0) * rateByDetail_);
    do {
        const float late = static_cast<float>(clock_ - cycle_) * block_.interval;
        if (late < block_.life[1]) {
            for (std::int64_t i = 0; i < count; ++i) {
                if (!spawn(late)) {
                    break;
                }
            }
        }
        ++cycle_;
        if (block_.burstLimit != 0 && block_.burstLimit <= cycle_) {
            stop(false);
            return;
        }
        if (block_.gap[0] >= 0) {
            clock_ -= static_cast<double>(random(block_.gap[0], block_.gap[1])) * clockRate_;
            if (clock_ < cycle_) {
                wait_ = static_cast<float>(cycle_ - clock_) * block_.interval;
                clock_ = cycle_;
                return;
            }
        }
    } while (static_cast<double>(cycle_) <= clock_ && --budget_ > 0);
}

// Makes particles at a rate that follows the count curve over a cycle, in steps of at most 1/15 second.
void MetaParticleEmitter::flow(float seconds) {
    const Block& b = block_;
    float rate = rateByDetail_;
    const float s = own_.scale;
    if (b.flags & Block::kScaleWithSize) {
        rate *= s;
    } else if (b.flags & Block::kScaleWithArea) {
        rate *= s * s;
    } else if (b.flags & Block::kScaleWithVolume) {
        rate *= s * s * s;
    }
    bool gapping = false;
    while (seconds > 0 && --budget_ > 0) {
        float h = std::min(seconds, kLongestStep);
        clock_ += static_cast<double>(h) * clockRate_;
        if (clock_ > static_cast<double>(cycle_) + 1) {
            ++cycle_;
            if (b.burstLimit != 0 && cycle_ == b.burstLimit) {
                if (!(b.flags & Block::kSustain)) {
                    stop(false);
                    return;
                }
                cycle_ = b.burstLimit - 1;
                clock_ = static_cast<float>(b.burstLimit) - 1e-6f;
            } else if (b.gap[0] >= 0) {
                gapping = true;
                h -= static_cast<float>(clock_ - cycle_) * b.interval;
                clock_ = cycle_;
            }
        }
        owed_ += b.count.at(static_cast<float>(clock_ - cycle_)) * h * rate;
        const float whole = std::floor(owed_);
        owed_ -= whole;
        const auto count = static_cast<std::int64_t>(std::clamp(whole, 0.0f, static_cast<float>(kMostParticles)));
        if (count > 0) {
            const float apart = h / static_cast<float>(count);
            float age = seconds;
            for (std::int64_t i = 0; i < count; ++i) {
                if (!spawn(age)) {
                    break;
                }
                age -= apart;
            }
        }
        seconds -= h;
        if (gapping) {
            gapping = false;
            seconds -= random(b.gap[0], b.gap[1]);
            if (seconds < 0) {
                wait_ = -seconds;
                return;
            }
        }
    }
}

void MetaParticleEmitter::emit(float seconds) {
    if (stopping_) {
        return;
    }
    if (wait_ > 0) {
        wait_ -= seconds;
        if (wait_ > 0) {
            return;
        }
        seconds = -wait_;
    }
    if (block_.prewarm > 0) {
        seconds = std::min(seconds, block_.prewarm - 0.01f);
    }
    if (block_.flags & Block::kHoldCount) {
        hold(seconds);
    } else if (block_.flags & Block::kBursts) {
        burst(seconds);
    } else {
        flow(seconds);
    }
}

// Turns a particle's effect so its y axis points along its flight, with x level and z the rest. With
// smoothing it only turns about the vertical, easing toward its heading by a share of the way each update
// (not each second). A particle at rest points nowhere and keeps x along y and z along x.
void MetaParticleEmitter::alignAlongFlight(Particle& p, std::array<float, 9>& r) const {
    const Vec3& v = p.velocity;
    if (block_.alignSmoothing > 0) {
        const float toward = std::atan2(-v[0], v[1]) / kTwoPi;
        float& turn = p.alignTurn;
        if (turn == kNoAlignTurn) {
            turn = toward;
        } else {
            if (toward - turn > 0.5f) {
                turn += 1;
            } else if (toward - turn < -0.5f) {
                turn -= 1;
            }
            turn += (toward - turn) / block_.alignSmoothing * kAlignEase;
            if (turn > 0.5f) {
                turn -= 1;
            } else if (turn < -0.5f) {
                turn += 1;
            }
        }
        r = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        turnAboutVertical(r, kTwoPi * turn);
        return;
    }
    Vec3 y = v;
    if (const float length = std::sqrt(lengthSquared(v)); length > 0) {
        for (float& c : y) {
            c /= length;
        }
    }
    Vec3 x{0, 1, 0}, z{1, 0, 0};
    if (const float flat = std::sqrt(y[0] * y[0] + y[1] * y[1]); flat > 0) {
        x = {y[1] / flat, -y[0] / flat, 0};
        z = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
    }
    r = {x[0], y[0], z[0], x[1], y[1], z[1], x[2], y[2], z[2]};
}

// Each living particle's effect goes where the particle is, turned and sized for its age.
void MetaParticleEmitter::moveEffects() {
    for (Particle& p : particles_) {
        if (!p.effect || !p.effect->running()) {
            continue;
        }
        const float t = p.age / p.life;
        EmitterPlace at;
        at.position = p.position;
        const float waveTime = static_cast<float>(block_.interval * played_);
        for (const Block::Wave& wave : block_.waves) {
            float angle = waveTime * wave.frequency;
            for (std::size_t i = 0; i < 3; ++i) {
                angle += wave.phase[i] * (p.position[i] - own_.position[i]);
            }
            const float s = std::sin(angle);
            for (std::size_t i = 0; i < 3; ++i) {
                at.position[i] += wave.amplitude[i] * s;
            }
        }
        at.scale = block_.size.at(t) * p.size;
        if (block_.alignment == kAlignY) {
            alignAlongFlight(p, at.rotation);
        } else {
            turnAboutVertical(at.rotation, kTwoPi * p.heading);
        }
        turnAboutVertical(at.rotation, block_.rotation.at(t) * p.rotation * kTwoPi);
        const Vec3 colour = colourAt(block_.colours, t);
        p.effect->setMessages(block_.alpha.at(t) * alpha_ * p.alpha, {colour[0] * p.colour[0], colour[1] * p.colour[1], colour[2] * p.colour[2]});
        p.effect->placeIn(at, parent_);
    }
}

void MetaParticleEmitter::update(float seconds, const EffectConditions& conditions) {
    if (!running_ || !playable_) {
        return;
    }
    if (prewarmNext_) {
        seconds += block_.prewarm;
        prewarmNext_ = false;
    }
    budget_ = kMostStepsAnUpdate;
    played_ += seconds;
    terrain_ = conditions.terrain;
    if (!terrain_ && (block_.flags & kTerrainFlags)) {
        diag::warnOnce("effects: metaparticles meet the ground only in the neighborhood so far (" + block_.effect + ")");
    }
    for (std::size_t i = 0; i < 3; ++i) {
        acceleration_[i] = block_.gravity[i] + conditions.wind[i] * block_.windShare;
    }
    if (block_.flags & Block::kWanderAll) {
        for (const Pull& pull : sharedPulls_) {
            addForce(pull, acceleration_);
        }
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        if (step(particles_[i], seconds)) {
            if (kept != i) {
                particles_[kept] = std::move(particles_[i]);
            }
            ++kept;
        } else {
            finish(particles_[i]);
        }
    }
    particles_.resize(kept);

    if (seconds > 0) {
        emit(seconds);
    }
    moveEffects();
    std::erase_if(dying_, [](const std::shared_ptr<PlayingEffect>& effect) { return !effect->running(); });
    if (stopping_ && particles_.empty() && dying_.empty()) {
        running_ = false;
        return;
    }
    if (block_.flags & Block::kWanderAll) {
        for (std::size_t k = 0; k < sharedPulls_.size(); ++k) {
            sharedPulls_[k].timer -= seconds;
            for (int changes = 0; sharedPulls_[k].timer < 0 && changes < 1000; ++changes) {
                choosePull(sharedPulls_[k], block_.wander[k]);
            }
        }
    }
}

} // namespace ts2::scene
