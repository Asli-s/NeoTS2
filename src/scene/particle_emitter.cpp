#include "scene/particle_emitter.hpp"

#include "diag/log.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ts2::scene {

namespace {

using Block = effects::ParticleBlock;
using Vec3 = std::array<float, 3>;

constexpr float kTwoPi = 6.28318530717958647692f;

// The longest step particles and flowing emission take, in seconds.
constexpr float kLongestStep = 1.0f / 15.0f;

// The shortest cycle a block can have, in seconds, so a cycle of 0 cannot divide by zero.
constexpr float kShortestCycle = 1e-6f;

// A particle moves in one step unless its drag over that step passes this share of its speed.
constexpr float kOneStepDrag = 0.05f;

// A bursting block of more particles than this follows the density setting. Smaller ones are
// always played in full, and make exactly `count` particles at any message rate.
constexpr float kAlwaysFullBurst = 20;

// An emitter moved further than this between updates jumped, so new particles aren't spread along the way.
constexpr float kLongestMoveSquared = 1;

// Blocks with these flags need small steps: collisions (0x8, 0x10), terrain collide (0x8000), terrain repel
// (0x20000) and attractors (0x4000000).
constexpr std::uint64_t kSmallStepFlags = 0x8 | 0x10 | 0x8000 | 0x20000 | 0x4000000;

// Flags for behaviour not played yet: those above but attractors, offsetting by the stretch (0x20), sticking to the
// terrain (0x10000), dying off it (0x40000), pinning to it (0x80000), and screw and path motion (the high bits).
constexpr std::uint64_t kNotPlayedFlags = (kSmallStepFlags & ~Block::kAttractor) | 0x20 | 0x10000 | 0x40000 | 0x80000 | 0x600000000ull | 0x2000000000ull;

// Limits that keep a broken file from taking over the game. The first is the particle target of Graphic
// Effects at High, which all effects together aim for. The second allows a 30000 second prewarm, the longest
// the game's files have, at the longest step.
constexpr std::size_t kMostParticles = 10000;
constexpr int kMostStepsAnUpdate = 500000;
constexpr int kMostWalkChanges = 1000;

bool finite(float v) { return std::isfinite(v); }

template <std::size_t N>
bool finite(const std::array<float, N>& values) {
    return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); });
}

bool finite(const effects::Curve& curve) { return std::all_of(curve.values.begin(), curve.values.end(), [](float v) { return std::isfinite(v); }); }

// False for a block with values that aren't numbers, or times too long to play. A broken file's block makes no particles.
bool playable(const Block& b) {
    constexpr float kLongest = 1e6f;
    return finite(b.life) && finite(b.prewarm) && finite(b.delay) && finite(b.gap) && finite(b.directionLow) && finite(b.directionHigh) &&
           finite(b.speed) && finite(b.spawnLow) && finite(b.spawnHigh) && finite(b.ring) && finite(b.count) && finite(b.interval) && finite(b.speedScale) &&
           finite(b.sizeVary) && finite(b.aspectVary) && finite(b.rotationScale) && finite(b.pivotVary) && finite(b.alphaVary) && finite(b.colourVary) &&
           finite(b.gravity) && finite(b.windShare) && finite(b.blast) && finite(b.blastCentre) && finite(b.drag) && finite(b.walk.retime) &&
           finite(b.walk.speed) && finite(b.walk.turn) && finite(b.walk.leastTurn) &&
           std::all_of(b.attractors.begin(), b.attractors.end(), [](const Block::Attractor& a) {
               return finite(a.point) && finite(a.strength) && finite(a.range) && finite(a.killRadius);
           }) && std::abs(b.prewarm) <= kLongest && std::abs(b.life[0]) <= kLongest &&
           std::abs(b.life[1]) <= kLongest;
}

float lengthSquared(const Vec3& v) { return v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; }

Vec3 turned(const Vec3& v, const std::array<float, 9>& r) {
    return {r[0] * v[0] + r[1] * v[1] + r[2] * v[2], r[3] * v[0] + r[4] * v[1] + r[5] * v[2], r[6] * v[0] + r[7] * v[1] + r[8] * v[2]};
}

// How many whole particles a count asks for: any part of one makes a whole one.
std::int64_t wholeCount(float count) {
    return static_cast<std::int64_t>(std::ceil(std::clamp(count, 0.0f, static_cast<float>(kMostParticles))));
}

} // namespace

ParticleEmitter::ParticleEmitter(const Block& block, std::uint32_t seed)
    : block_(block), playable_(playable(block)), randomState_(seed == 0 ? 1 : seed), firstFrame_(block.firstFrame) {
    clockRate_ = 1.0 / std::max(block.interval, kShortestCycle);
    if (!playable_) {
        diag::warnOnce("effects: a particle system with values out of range makes no particles (" + block.texture + ")");
    }
    if (block.flags & kNotPlayedFlags) {
        diag::warnOnce("effects: a particle system uses motion not played yet (" + block.texture + ")");
    }
}

// A random number from low to high.
float ParticleEmitter::random(float low, float high) {
    randomState_ ^= randomState_ << 13;
    randomState_ ^= randomState_ >> 17;
    randomState_ ^= randomState_ << 5;
    const float share = static_cast<float>(randomState_ - 1) / 4294967296.0f;
    return low + (high - low) * share;
}

void ParticleEmitter::start(bool prewarm) {
    if (prewarm && block_.burstLimit != 0 && !(block_.flags & Block::kSustain)) {
        return;
    }
    if (!running_ || stopping_) {
        stopping_ = false;
        running_ = true;
    }
    clock_ = 0;
    lastPosition_ = place_.position;
    cycle_ = 0;
    owed_ = 0;
    acceleration_ = block_.gravity;
    walkForce_ = {};
    hardStopped_ = false;
    prewarmNext_ = prewarm;
    if (block_.flags & Block::kRandomWalk) {
        walkTimer_ = 0;
        walkHeadings_ = {random(-0.5f, 0.5f), random(-0.5f, 0.5f)};
        walk(0);
    }
    if (block_.delay[0] >= 0) {
        wait_ = random(block_.delay[0], block_.delay[1]);
    } else if (block_.gap[0] >= 0) {
        wait_ = random(block_.gap[0], block_.gap[1]);
    } else {
        wait_ = 0;
    }
}

void ParticleEmitter::stop(bool hard) {
    if (!running_) {
        return;
    }
    hardStopped_ = hard;
    stopping_ = !hard;
    if (hard) {
        running_ = false;
        removeAll();
    }
}

std::vector<std::array<float, 3>> ParticleEmitter::takeDeaths() {
    std::vector<std::array<float, 3>> out;
    out.swap(deaths_);
    return out;
}

// A push that changes each time its timer runs out. Each change turns both headings, in full turns, by a random
// amount of at least the block's least turn.
void ParticleEmitter::walk(float seconds) {
    const Block::Walk& w = block_.walk;
    walkTimer_ -= seconds;
    // A retime of 0 or close to it could keep this loop going for good, so a change is the last one after many.
    for (int changes = 0; walkTimer_ <= 0; ++changes) {
        walkTimer_ = changes < kMostWalkChanges ? walkTimer_ + random(w.retime[0], w.retime[1]) : 1;
        for (float& heading : walkHeadings_) {
            const float change = random(-w.turn, w.turn);
            heading += change;
            heading += change >= 0 ? w.leastTurn : -w.leastTurn;
        }
        const float strength = random(w.speed[0], w.speed[1]);
        const float h1 = kTwoPi * walkHeadings_[0], h2 = kTwoPi * walkHeadings_[1];
        walkForce_ = {std::cos(h1) * std::cos(h2) * strength, std::sin(h1) * std::cos(h2) * strength, std::sin(h2) * strength};
    }
}

// A point in the spawn box, or the ball or torus that fills it. The ball and torus are picked by rejection in -1..1.
std::array<float, 3> ParticleEmitter::randomSpawnPoint() {
    const Vec3& low = block_.spawnLow;
    const Vec3& high = block_.spawnHigh;
    if (block_.ring < 0 && !(block_.flags & Block::kBall)) {
        return {random(low[0], high[0]), random(low[1], high[1]), random(low[2], high[2])};
    }
    Vec3 unit{};
    // Rejection almost always ends in a few tries, and the last point is used if it never does.
    for (int tries = 0; tries < 1000; ++tries) {
        unit = {random(-1, 1), random(-1, 1), random(-1, 1)};
        if (block_.ring >= 0) {
            const float flat = unit[0] * unit[0] + unit[1] * unit[1];
            const float off = std::sqrt(flat) - 0.5f;
            if (0.25f - off * off >= unit[2] * unit[2] && flat > 0) {
                const float k = (1 - block_.ring) / std::sqrt(flat) + block_.ring;
                unit = {unit[0] * k, unit[1] * k, unit[2] * block_.ring};
                break;
            }
        } else if (lengthSquared(unit) < 1) {
            break;
        }
    }
    Vec3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        out[i] = (high[i] + low[i] + (high[i] - low[i]) * unit[i]) * 0.5f;
    }
    return out;
}

const std::string& ParticleEmitter::texture() const {
    return (block_.flags & Block::kAcceptsPicture) && !picture_.empty() ? picture_ : block_.texture;
}

// A new particle in the world: a position scaled and placed by the emitter, a velocity only turned by it.
Particle ParticleEmitter::makeParticle() {
    const Block& b = block_;
    Particle p;
    p.life = random(b.life[0], b.life[1]);
    p.firstFrame = firstFrame_;
    p.position = randomSpawnPoint();

    Vec3 direction = (b.flags & Block::kDirectionIsSpawn)
                         ? p.position
                         : Vec3{random(b.directionLow[0], b.directionHigh[0]), random(b.directionLow[1], b.directionHigh[1]),
                                random(b.directionLow[2], b.directionHigh[2])};
    const float speed = random(b.speed[0], b.speed[1]) * scales_.speed;
    const float squared = lengthSquared(direction);
    // No direction at all flies along x.
    if (squared > 1.4e-20f) {
        const float k = speed / std::sqrt(squared);
        p.velocity = {direction[0] * k, direction[1] * k, direction[2] * k};
    } else {
        p.velocity = {speed, 0, 0};
    }
    if (b.blast != 0) {
        const Vec3 away{p.position[0] - b.blastCentre[0], p.position[1] - b.blastCentre[1], p.position[2] - b.blastCentre[2]};
        const float distance = lengthSquared(away);
        if (distance > 0) {
            const float k = b.blast / std::sqrt(distance);
            for (std::size_t i = 0; i < 3; ++i) {
                p.velocity[i] += away[i] * k;
            }
        }
    }
    p.position = turned(p.position, place_.rotation);
    for (std::size_t i = 0; i < 3; ++i) {
        p.position[i] = p.position[i] * place_.scale + place_.position[i];
    }
    p.velocity = turned(p.velocity, place_.rotation);

    p.size = random(1 - b.sizeVary, 1 + b.sizeVary) * newSize_;
    p.aspect = random(1 - b.aspectVary, 1 + b.aspectVary);
    p.rotation = b.rotationScale[1] <= b.rotationScale[0] ? b.rotationScale[0] : random(b.rotationScale[0], b.rotationScale[1]);
    p.pivot = {0.5f + random(-b.pivotVary[0], b.pivotVary[0]), 0.5f + random(-b.pivotVary[1], b.pivotVary[1]), random(-b.pivotVary[2], b.pivotVary[2])};
    p.alpha = random(1 - b.alphaVary, 1 + b.alphaVary);
    for (std::size_t i = 0; i < 3; ++i) {
        p.colour[i] = random(1 - b.colourVary[i], 1 + b.colourVary[i]);
    }
    return p;
}

// Ages a particle and moves it. False when it died of age: a dead particle doesn't move any more.
bool ParticleEmitter::step(Particle& p, float seconds) {
    p.age += seconds;
    if (p.age >= p.life) {
        return false;
    }
    const float drag = block_.drag;
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
        const float keep = 1 - h * drag;
        for (std::size_t i = 0; i < 3; ++i) {
            p.position[i] += p.velocity[i] * h;
        }
        Vec3 force{};
        const bool alive = !(block_.flags & Block::kAttractor) || pull(p, force);
        for (std::size_t i = 0; i < 3; ++i) {
            p.velocity[i] = (p.velocity[i] + (acceleration_[i] + force[i]) * h) * keep;
        }
        if (!alive) {
            return false;
        }
        seconds -= h;
    }
    return true;
}

// The attractors of type 1 move with the emitter.
void ParticleEmitter::placeAttractors() {
    attractorPoints_.resize(block_.attractors.size());
    for (std::size_t a = 0; a < block_.attractors.size(); ++a) {
        const Block::Attractor& attractor = block_.attractors[a];
        attractorPoints_[a] = attractor.point;
        if (attractor.type == 1) {
            attractorPoints_[a] = turned(attractor.point, place_.rotation);
            for (std::size_t i = 0; i < 3; ++i) {
                attractorPoints_[a][i] = attractorPoints_[a][i] * place_.scale + place_.position[i];
            }
        }
    }
}

// Adds up the attractors' pull on a particle. False when it came too close to one and dies.
bool ParticleEmitter::pull(const Particle& p, Vec3& force) {
    for (std::size_t a = 0; a < attractorPoints_.size(); ++a) {
        const Block::Attractor& attractor = block_.attractors[a];
        const Vec3& point = attractorPoints_[a];
        const Vec3 away{p.position[0] - point[0], p.position[1] - point[1], p.position[2] - point[2]};
        Vec3 towards{-away[0], -away[1], -away[2]};
        if (attractor.type == 2) {
            towards[0] = 0;
        } else if (attractor.type == 3) {
            towards[1] = towards[2] = 0;
        } else if (attractor.type == 4) {
            const float distance = std::sqrt(lengthSquared(away));
            if (distance == 0) {
                // At the very centre any way out will do.
                towards = {1, 0, 0};
            } else {
                const float k = (1 - distance) / distance;
                towards = {away[0] * k, away[1] * k, away[2] * k};
            }
        }
        const float squared = lengthSquared(towards);
        if (squared <= 0) {
            if (attractor.killRadius > 0) {
                return false;
            }
            continue;
        }
        const float distance = std::sqrt(squared);
        if (distance < attractor.killRadius) {
            return false;
        }
        const float share = std::min(distance / attractor.range, 1.0f);
        const float k = (share < 0 ? 0 : attractor.strength.at(share)) / distance;
        for (std::size_t i = 0; i < 3; ++i) {
            force[i] += towards[i] * k;
        }
    }
    return true;
}

void ParticleEmitter::finish(const Particle& p) {
    if (!hardStopped_ && !block_.deathEffect.empty() && deaths_.size() < kMostParticles) {
        deaths_.push_back(p.position);
    }
}

void ParticleEmitter::removeAll() {
    for (const Particle& p : particles_) {
        finish(p);
    }
    particles_.clear();
}

// Adds a new particle that started `seconds` ago, unless that is longer than its life.
void ParticleEmitter::addAged(Particle p, float seconds) {
    if (particles_.size() >= kMostParticles) {
        diag::warnOnce("effects: a particle system reached the most particles one may have (" + block_.texture + ")");
        return;
    }
    if (!step(p, seconds)) {
        finish(p);
        return;
    }
    particles_.push_back(p);
}

// Keeps the count alive, replacing the dead. The density setting has no say.
void ParticleEmitter::hold(float seconds) {
    clock_ += seconds * clockRate_;
    std::int64_t missing = wholeCount(block_.count.at(0)) - static_cast<std::int64_t>(particles_.size());
    for (; missing > 0; --missing) {
        addAged(makeParticle(), seconds);
    }
}

// Makes the count at the start of every cycle, each one as old as the cycle is late.
void ParticleEmitter::burst(float seconds) {
    clock_ += seconds * clockRate_;
    if (clock_ < cycle_) {
        return;
    }
    const std::int64_t count = wholeCount(block_.count.at(0) * rate_);
    do {
        if (block_.flags & Block::kCycleKill) {
            removeAll();
        }
        const float late = static_cast<float>(clock_ - cycle_) * block_.interval;
        if (late < block_.life[1]) {
            for (std::int64_t i = 0; i < count; ++i) {
                addAged(makeParticle(), late);
            }
        }
        ++cycle_;
        if (block_.burstLimit != 0 && std::uint32_t{block_.burstLimit} <= cycle_) {
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
// New particles spread back along the way the emitter moved, and over the step's time.
void ParticleEmitter::flow(float seconds) {
    if (seconds <= 0) {
        return;
    }
    const Block& b = block_;
    float rate = rate_;
    const float s = place_.scale;
    if (b.flags & Block::kScaleWithSize) {
        rate *= s;
    } else if (b.flags & Block::kScaleWithArea) {
        rate *= s * s;
    } else if (b.flags & Block::kScaleWithVolume) {
        rate *= s * s * s;
    }
    const Vec3 move{place_.position[0] - lastPosition_[0], place_.position[1] - lastPosition_[1], place_.position[2] - lastPosition_[2]};
    const float all = seconds;
    if (b.speedScale > 0) {
        rate *= std::sqrt(lengthSquared(move)) / seconds * b.speedScale + 1;
    }
    bool gapping = false;
    while (--budget_ > 0) {
        float h = std::min(seconds, kLongestStep);
        clock_ += static_cast<double>(h) * clockRate_;
        if (clock_ > static_cast<double>(cycle_) + 1) {
            ++cycle_;
            if (b.flags & Block::kCycleKill) {
                removeAll();
            }
            if (b.burstLimit != 0 && cycle_ == std::uint32_t{b.burstLimit}) {
                if (!(b.flags & Block::kSustain)) {
                    stop(false);
                    return;
                }
                cycle_ = b.burstLimit - 1u;
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
                Particle p = makeParticle();
                const float back = age / all;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    p.position[axis] -= move[axis] * back;
                }
                addAged(p, age);
                age -= apart;
            }
        }
        seconds -= h;
        if (gapping) {
            seconds -= random(b.gap[0], b.gap[1]);
            if (seconds < 0) {
                wait_ = -seconds;
                return;
            }
        }
        if (seconds <= 0) {
            return;
        }
    }
}

void ParticleEmitter::emit(float seconds) {
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

void ParticleEmitter::update(float seconds, const EffectConditions& conditions) {
    if (!running_ || !playable_) {
        return;
    }
    if (prewarmNext_) {
        seconds += block_.prewarm;
        prewarmNext_ = false;
    }
    if (!(seconds > 0)) {
        return;
    }
    budget_ = kMostStepsAnUpdate;
    const Vec3 now = place_.position;
    const Vec3 moved{now[0] - lastPosition_[0], now[1] - lastPosition_[1], now[2] - lastPosition_[2]};
    if (lengthSquared(moved) > kLongestMoveSquared) {
        lastPosition_ = now;
    }

    rate_ = scales_.rate;
    if (block_.flags & Block::kScaleExisting) {
        liveSize_ = scales_.size;
        newSize_ = 1;
    } else {
        newSize_ = scales_.size;
        liveSize_ = 1;
    }
    if (block_.flags & Block::kScaleParticles) {
        newSize_ *= place_.scale;
    }
    if ((block_.flags & Block::kBursts) && block_.count.at(0) <= kAlwaysFullBurst) {
        rate_ = 1;
    } else if ((block_.flags & Block::kBursts) || !(block_.flags & Block::kHoldCount)) {
        newSize_ *= conditions.particleScale;
        rate_ *= conditions.density;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        acceleration_[i] = block_.gravity[i] + walkForce_[i] + conditions.wind[i] * block_.windShare;
    }

    if (block_.flags & Block::kAttractor) {
        placeAttractors();
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        if (step(particles_[i], seconds)) {
            if (kept != i) {
                particles_[kept] = particles_[i];
            }
            ++kept;
        } else {
            finish(particles_[i]);
        }
    }
    particles_.resize(kept);

    emit(seconds);

    if (particles_.empty() && stopping_) {
        running_ = false;
        return;
    }
    if (block_.flags & Block::kRandomWalk) {
        walk(seconds);
    }
    lastPosition_ = now;
}

} // namespace ts2::scene
