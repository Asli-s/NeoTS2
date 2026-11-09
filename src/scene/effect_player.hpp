#pragma once

#include "formats/effects.hpp"
#include "scene/meta_particles.hpp"
#include "scene/particle_emitter.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::scene {

// Plays visual effects by name: it picks the record for the Graphic Effects level, makes the parts the
// level of detail and the game's state allow, places them and ends the effect when they finish.
// Played parts: particle systems, metaparticles, sequences, models and other effects by name. Other block types play
// without those parts and say so in the log, and so do effects attached to the camera or the cursor, or kept to a
// size on the screen.

class EffectPlayer;
struct EffectPart;

class PlayingEffect {
public:
    PlayingEffect(const PlayingEffect&) = delete;
    PlayingEffect& operator=(const PlayingEffect&) = delete;
    ~PlayingEffect();

    // Starts the effect's parts. With `prewarm` their first update also plays their prewarm seconds, so a hard
    // start from a script shows them grown at once. Parts made while it runs start without a prewarm.
    void start(bool prewarm);

    // A soft stop lets the parts finish. A hard stop removes them at once. An effect with the hardStop keyword
    // always stops hard.
    void stop(bool hard);

    // Where the effect sits. Effects keep particles where they were made unless they have the rigid keyword,
    // which carries them along.
    void place(const EmitterPlace& where) { placeIn(where, EmitterPlace{}); }

    // Where the effect sits inside `parent`, whose moves carry along what it has already made.
    void placeIn(const EmitterPlace& own, const EmitterPlace& parent);

    // The alpha and colour the effect's particles are multiplied by, unless a keyword refuses them.
    void setMessages(float alpha, const std::array<float, 3>& colour);

    // A texture for the parts that take one, and the atlas frame their particles start at, as balloons show a
    // script's picture. They reach its other effects and those its metaparticles make later, and parts made again
    // keep them. Sequences don't pass them on. An effect or part with the keyword refusing messages ignores them.
    void setPicture(const std::string& name);
    void setFirstFrame(int frame);

    // A hidden effect goes on playing, but nothing of it is drawn.
    void setHidden(bool hidden);
    bool hidden() const { return hidden_; }

    bool running() const { return running_; }
    bool stopping() const { return stopping_; }

private:
    friend class EffectPlayer;
    friend struct EffectPart;

    PlayingEffect(EffectPlayer& player, std::string name, const effects::VisualEffect* record, int nesting);

    void update(float seconds);
    bool finished();

    EffectPlayer* player_;
    std::string name_;
    const effects::VisualEffect* record_;
    int nesting_;  // how many effects it is inside of
    std::vector<EffectPart> parts_;
    EmitterPlace ownGiven_, parentGiven_;  // as placed, before its keywords
    EmitterPlace own_, parent_;
    std::array<float, 2> sizeByDetail_{-1, -1};  // set when it is part of another effect
    std::string picture_;
    int firstFrame_ = -1;  // below 0: its parts keep their own
    float messageAlpha_ = 1;
    std::array<float, 3> messageColour_{1, 1, 1};
    bool nested_ = false;
    bool hidden_ = false;
    bool running_ = false;
    bool stopping_ = false;
    bool placedSinceStart_ = false;
};

// Plays visual effects one after another, each by itself and for a time from its item's range.
class EffectSequence {
public:
    // Keeps references to `block` and `player`, which must outlive it.
    EffectSequence(const effects::SequenceBlock& block, EffectPlayer& player, std::uint32_t seed, int nesting);

    void start(bool prewarm);
    void stop(bool hard);
    void update(float seconds);
    void place(const EmitterPlace& own, const EmitterPlace& parent);
    void setHidden(bool hidden);

    // True until stopped and the effects it keeps watching have ended.
    bool running() const { return running_ || !kept_.empty(); }

private:
    float random(float low, float high);

    const effects::SequenceBlock& block_;
    EffectPlayer& player_;
    int nesting_;
    std::uint32_t randomState_;
    std::shared_ptr<PlayingEffect> current_;
    std::vector<std::shared_ptr<PlayingEffect>> kept_;  // earlier items' effects, still finishing
    EmitterPlace own_, parent_;
    std::size_t item_ = 0;
    float time_ = 0, endTime_ = 0;  // seconds
    bool untilEffectEnds_ = false;
    bool hidden_ = false;
    bool running_ = false;
    bool fresh_ = true;
};

// One made part of a playing effect.
struct EffectPart {
    const effects::Component* component = nullptr;
    std::unique_ptr<ParticleEmitter> emitter;
    std::unique_ptr<MetaParticleEmitter> meta;
    std::unique_ptr<EffectSequence> sequence;
    std::shared_ptr<PlayingEffect> child;
    const effects::ModelBlock* model = nullptr;
    bool modelShown = false;  // from its start until it is stopped
    EmitterPlace parent;  // the emitter's parent place, which carries its particles, or a model's place in the world
    std::array<float, 4> modelColour{1, 1, 1, 1};  // red, green, blue and alpha
    std::array<float, 2> rate{1, 1}, size{1, 1}, alpha{1, 1};  // by level of detail: this level's and the next's

    void start(bool prewarm);
    void stop(bool hard);
    bool running() const;
    void setPlace(const EmitterPlace& own, const EmitterPlace& parentPlace);
    void setPicture(const std::string& name);
    void setFirstFrame(int frame);
    // A model's colour and alpha: its block's times the effect's messages.
    void setModelMessages(float messageAlpha, const std::array<float, 3>& colour);
};

class EffectPlayer {
public:
    // Keeps a reference to `file`, which must outlive the player. `seed` picks its random values.
    EffectPlayer(const effects::File& file, std::uint32_t seed);
    ~EffectPlayer();
    EffectPlayer(const EffectPlayer&) = delete;
    EffectPlayer& operator=(const EffectPlayer&) = delete;

    // A new effect, not started yet. Null for a name the file doesn't have. A name with no record at this
    // level gives an effect that never plays. The player keeps an effect until it has finished and only the
    // player holds it, so dropping the pointer of a playing effect lets it end by itself.
    std::shared_ptr<PlayingEffect> create(std::string_view name);

    // True when the file has this name at any level.
    bool knows(std::string_view name) const { return !name.empty() && file_.knows(name); }

    // As create, for an effect started by another that is `nesting` effects deep. Null at the nesting limit, the
    // guard against effects that name each other.
    std::shared_ptr<PlayingEffect> createInside(std::string_view name, int nesting);

    // The Graphic Effects level (effectPriorityLevel in Graphics Rules). A change stops every effect at
    // once and plays its record for the new level.
    void setLevel(std::uint16_t level);
    // The particles effects try to stay under (maxParticlesTarget in Graphics Rules). Above it new particles thin out
    // by target / count on every update, and they come back as the count falls.
    void setParticleTarget(std::uint32_t target) { particleTarget_ = target; }
    std::size_t particleCount() const;

    // The camera's zoom, which sets the level of detail, held from 1 to just below 5. Whole steps start and stop
    // parts, and what is between them blends the parts' rate, size and alpha.
    void setDetail(float zoom);

    // Where the camera is and looks, in the world, for effects that face it. Unit directions.
    void setCamera(const std::array<float, 3>& eye, const std::array<float, 3>& forward, const std::array<float, 3>& up);

    // The game's state bits that parts ask for. Takes effect at the next update.
    void setState(std::uint32_t bits) { nextState_ = bits; }

    void setConditions(const EffectConditions& conditions) { conditions_ = conditions; }
    const EffectConditions& conditions() const { return conditions_; }

    // Moves every effect on. Effects run on `gameSeconds` unless they have the realTime keyword.
    void update(float gameSeconds, float realSeconds);

    // Calls `visit` for each particle system with its block, and the parent place to draw its particles by.
    using ParticleVisitor = std::function<void(const effects::ParticleBlock&, const ParticleEmitter&, const EmitterPlace&)>;
    void forEachParticleSystem(const ParticleVisitor& visit) const;

    // Calls `visit` for each model shown with its block, where it is and its colour and alpha (all 1 for none).
    using ModelVisitor = std::function<void(const effects::ModelBlock&, const EmitterPlace&, const std::array<float, 4>&)>;
    void forEachModel(const ModelVisitor& visit) const;

    std::size_t effectCount() const { return effects_.size(); }

private:
    friend class PlayingEffect;

    const effects::File& file_;
    std::vector<std::shared_ptr<PlayingEffect>> effects_;
    EffectConditions conditions_;
    std::uint32_t particleTarget_ = 10000;
    bool hasCamera_ = false;
    std::array<float, 3> eye_{}, forward_{}, up_{};
    std::uint32_t randomState_ = 1;
    std::uint16_t level_ = effects::kHighestLevel;
    bool levelChanging_ = false;
    std::uint32_t state_ = 0, stateBefore_ = 0, nextState_ = 0;
    int detail_ = -1, detailBefore_ = -1;
    float zoom_ = -1, zoomBefore_ = 0;
    float detailShare_ = 0;  // how far between this level of detail and the next

    std::uint32_t randomBits();
    std::shared_ptr<PlayingEffect> make(std::string_view name, int nesting);
    void rebuild(PlayingEffect& effect, bool force, std::uint32_t changedState);
    void makePart(PlayingEffect& effect, const effects::Component& component, bool force, bool prewarm);
    void rebuildAll();
    void setPartDetail(EffectPart& part) const;
    void updateParticles(EffectPart& part, float seconds, float alpha, const std::array<float, 3>& colour);
};

} // namespace ts2::scene
