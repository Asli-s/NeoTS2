#pragma once

#include "formats/effects.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ts2::scene {

// One particle system of a playing visual effect: it makes particles as its block says and moves
// them, with nothing drawn here. Not played yet: collisions, pinning to the floor, screw
// motion and paths. A block using them plays without them and says so in the log.

// Where an emitter sits: rows of a rotation, then a uniform scale, then a position.
struct EmitterPlace {
    std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    float scale = 1;
    std::array<float, 3> position{};
};

// The values an effect's messages and level of detail set on one emitter. All 1 means no change.
struct EmitterScales {
    float rate = 1;   // times the particles made
    float size = 1;   // times the particles' size
    float alpha = 1;
    float speed = 1;  // times new particles' speed
    std::array<float, 3> colour{1, 1, 1};
};

// What all effects share, from the Graphic Effects setting and the weather.
struct EffectConditions {
    float density = 1;        // particleDensity in Graphics Rules, lowered while over the particle target
    float particleScale = 1;  // particleScale in Graphics Rules
    std::array<float, 3> wind{};  // pushes in proportion to a block's windShare
};

struct Particle {
    float age = 0, life = 0;  // seconds
    std::array<float, 3> position{}, velocity{};
    std::uint8_t firstFrame = 0;  // of its atlas
    float size = 1, aspect = 1, rotation = 1, alpha = 1;  // its random factors, set when it starts
    std::array<float, 3> pivot{0.5f, 0.5f, 0};
    std::array<float, 3> colour{1, 1, 1};
};

class ParticleEmitter {
public:
    // Keeps a reference to `block`, which must outlive the emitter. `seed` picks its random values.
    ParticleEmitter(const effects::ParticleBlock& block, std::uint32_t seed);

    // Starts or restarts making particles. Particles still alive stay. With `prewarm` the first update also
    // plays the block's prewarm seconds. A restart with `prewarm` does nothing to a block with a cycle
    // limit unless it holds its last cycle.
    void start(bool prewarm);

    // A soft stop makes no more particles and lets the living finish. A hard stop removes them at once
    // and plays no death effects.
    void stop(bool hard);

    // Where it sits now. Placing it again before the next update moves new particles along the way.
    void place(const EmitterPlace& where) { place_ = where; }
    const EmitterPlace& where() const { return place_; }
    void setScales(const EmitterScales& scales) { scales_ = scales; }

    // The texture a script gave it, which a block with kAcceptsPicture draws instead of its own.
    void setPicture(const std::string& name) { picture_ = name; }
    const std::string& texture() const;

    // The atlas frame new particles start at, and that a frame cycle runs from.
    void setFirstFrame(int frame) { firstFrame_ = static_cast<std::uint8_t>(frame); }
    std::uint8_t firstFrame() const { return firstFrame_; }

    // Moves the particles on by `seconds`, then makes new ones.
    void update(float seconds, const EffectConditions& conditions);

    // True from start until stopped and every particle is gone.
    bool running() const { return running_; }
    const std::vector<Particle>& particles() const { return particles_; }

    // The size factor of particles already alive, which only a block with kScaleExisting changes.
    float liveSize() const { return liveSize_; }
    float alpha() const { return scales_.alpha; }
    const std::array<float, 3>& colour() const { return scales_.colour; }

    // Where particles with a death effect died since the last call, cleared by it.
    std::vector<std::array<float, 3>> takeDeaths();

private:
    float random(float low, float high);
    std::array<float, 3> randomSpawnPoint();
    Particle makeParticle();
    bool step(Particle& particle, float seconds);
    void addAged(Particle particle, float seconds);
    void finish(const Particle& particle);
    void removeAll();
    void hold(float seconds);
    void burst(float seconds);
    void flow(float seconds);
    void emit(float seconds);
    void walk(float seconds);
    void placeAttractors();
    bool pull(const Particle& particle, std::array<float, 3>& force);

    const effects::ParticleBlock& block_;
    bool playable_ = false;
    std::uint32_t randomState_ = 0;
    std::vector<Particle> particles_;
    std::vector<std::array<float, 3>> deaths_;
    EmitterPlace place_;
    EmitterScales scales_;
    std::array<float, 3> lastPosition_{};
    std::array<float, 3> acceleration_{};
    std::vector<std::array<float, 3>> attractorPoints_;  // where each attractor is now
    std::array<float, 3> walkForce_{};
    std::array<float, 2> walkHeadings_{};
    float walkTimer_ = 0;
    double clock_ = 0;       // cycles played
    double clockRate_ = 1;   // cycles per second
    std::uint32_t cycle_ = 0;
    float owed_ = 0;         // the part of a particle not yet made
    float wait_ = 0;         // seconds until it makes particles again
    float rate_ = 1;
    float newSize_ = 1;
    float liveSize_ = 1;
    int budget_ = 0;         // small steps left in this update, a guard against broken files
    std::string picture_;
    std::uint8_t firstFrame_ = 0;
    bool running_ = false;
    bool stopping_ = false;
    bool hardStopped_ = false;
    bool prewarmNext_ = false;
};

} // namespace ts2::scene
