#pragma once

#include "formats/effects.hpp"
#include "scene/particle_emitter.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace ts2::scene {

class EffectPlayer;
class PlayingEffect;

// A particle system whose particles are whole visual effects, by name: each one plays its effect where it
// flies, sized, turned and faded over its life. With a terrain, particles can start on the ground or the water,
// only at some depths, keep inside it, steer away from shallows and stay above the ground. Not played yet:
// attractors, screw motion, alignment along x or z and the screen's edge. A block using them plays without
// them and says so in the log.
class MetaParticleEmitter {
public:
    // Keeps references to `block` and `player`, which must outlive the emitter. `nesting` is how many
    // effects the emitter is inside of, which its particles' effects carry on from.
    MetaParticleEmitter(const effects::MetaParticleBlock& block, EffectPlayer& player, std::uint32_t seed, int nesting);

    // Starts or restarts making particles. Particles still alive stay. With `prewarm` the first update also
    // plays the block's prewarm seconds. A restart with `prewarm` does nothing to a block with a cycle
    // limit unless it holds its last cycle.
    void start(bool prewarm);

    // A soft stop makes no more particles and lets the living finish. A hard stop removes them and their
    // effects at once.
    void stop(bool hard);

    void place(const EmitterPlace& own, const EmitterPlace& parent);
    void setDetailScales(float rate, float size);
    void setAlpha(float alpha) { alpha_ = alpha; }

    // What the effects of particles it makes later are given. No picture and a frame below 0 give nothing.
    void setPicture(const std::string& name) { picture_ = name; }
    void setFirstFrame(int frame) { firstFrame_ = frame; }

    // Hides or shows the effects of its particles, those it makes later too.
    void setHidden(bool hidden);

    void update(float seconds, const EffectConditions& conditions);

    // True from start until stopped and every particle and its effect is gone.
    bool running() const { return running_; }
    std::size_t liveParticles() const { return particles_.size(); }

private:
    // A pull that changes by itself. See effects::MetaParticleBlock::Wander.
    struct Pull {
        float timer = 0, upwardStrength = 0, flatStrength = 0, yaw = 0, pitch = 0;
    };
    static constexpr float kNoAlignTurn = -999;
    struct Particle {
        float age = 0, life = 0;
        std::array<float, 3> position{}, velocity{};
        float size = 1, rotation = 1, alpha = 1, heading = 0;
        float alignTurn = kNoAlignTurn;  // full turns, for smoothed alignment along its flight
        std::array<float, 3> colour{1, 1, 1};
        std::array<Pull, 2> pulls;
        std::shared_ptr<PlayingEffect> effect;
    };

    float random(float low, float high);
    std::array<float, 3> randomSpawnPoint();
    void choosePull(Pull& pull, const effects::MetaParticleBlock::Wander& wander);
    void startPull(Pull& pull, const effects::MetaParticleBlock::Wander& wander, const std::array<float, 3>& direction);
    bool spawn(float agedBy);
    bool onTerrain(Particle& particle);
    bool step(Particle& particle, float seconds);
    bool meetTerrain(Particle& particle, float seconds);
    std::array<float, 3> toWorld(const std::array<float, 3>& at) const;
    std::array<float, 3> fromWorld(const std::array<float, 3>& at) const;
    std::array<float, 3> turnToWorld(const std::array<float, 3>& direction) const;
    std::array<float, 3> turnFromWorld(const std::array<float, 3>& direction) const;
    bool followPath(Particle& particle) const;
    void finish(Particle& particle);
    void emit(float seconds);
    void hold(float seconds);
    void burst(float seconds);
    void flow(float seconds);
    void alignAlongFlight(Particle& particle, std::array<float, 9>& rotation) const;
    void moveEffects();

    const effects::MetaParticleBlock& block_;
    EffectPlayer& player_;
    int nesting_;
    bool playable_ = false;
    std::uint32_t randomState_;
    std::vector<Particle> particles_;
    std::vector<std::shared_ptr<PlayingEffect>> dying_;  // effects of dead particles, still finishing
    std::array<Pull, 2> sharedPulls_;
    std::array<float, 3> acceleration_{};
    EmitterPlace own_, parent_;
    const EffectTerrain* terrain_ = nullptr;
    double clock_ = 0;      // cycles played
    double played_ = 0;     // seconds played since it was made, for the block's waves
    double clockRate_ = 1;  // cycles per second
    std::uint32_t cycle_ = 0;
    float owed_ = 0;   // the part of a particle not yet made
    float wait_ = 0;   // seconds until it makes particles again
    float rateByDetail_ = 1, sizeByDetail_ = 1;
    float alpha_ = 1;
    int budget_ = 0;   // small steps left in this update, a guard against broken files
    std::string picture_;
    int firstFrame_ = -1;
    bool running_ = false;
    bool stopping_ = false;
    bool hardStopped_ = false;
    bool prewarmNext_ = false;
    bool hidden_ = false;
};

} // namespace ts2::scene
