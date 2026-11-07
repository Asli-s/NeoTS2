#pragma once

#include "render/types.hpp"
#include "scene/effect_player.hpp"
#include "scene/particle_quads.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace ts2::scene {

// The effects lot objects are playing, by object, effect name, bone and number, as the Effect Stop/Start
// primitive and animation events start and stop them.
class ObjectEffects {
public:
    // What started an effect. A kind can be stopped as a group. Stopping AnyKind stops them all, and
    // animation events start their effects as that kind, so nothing else stops them as a group.
    enum Kind : std::uint8_t { AnyKind = 0, Primitive = 2, QueuedPrimitive = 5, OtherPrimitive = 7 };

    // Where an object's node or bone is now.
    struct Where {
        enum class State : std::uint8_t {
            NotReady,  // not drawn yet, as a Sim still loading: effects wait for it
            NoBone,    // the node has no such bone: a new effect doesn't start
            Gone,      // removed: its effects end at once
            Here,
        };
        State state = State::NotReady;
        bool hidden = false;  // its effects go on, unseen, and stay where they were
        EmitterPlace place;
    };
    using PlaceOf = std::function<Where(std::int16_t object, const std::string& bone)>;

    // Keeps a reference to `player`, which must outlive this.
    explicit ObjectEffects(EffectPlayer& player) : player_(player) {}

    // Starts the effect, soft or hard, making it if it isn't there. `keep` protects it from stops of all
    // of an object's effects. An effect on a bone the object doesn't have doesn't start.
    void start(std::int16_t object, const std::string& name, const std::string& bone, int number, Kind kind, bool keep, bool hard, const PlaceOf& placeOf);

    // Gives the effect, if it is there, a texture and first atlas frame for the parts that take them, as balloons
    // show a script's picture.
    void setPicture(std::int16_t object, const std::string& name, const std::string& bone, int number, const std::string& picture, int firstFrame);

    // Asks for the effect in the object's queue of effects that play in turn. The highest `priority` goes first, and
    // what is playing stays in front until it ends. With nothing queued it starts at once. It is dropped if its turn
    // hasn't come after tick `expires`. A picture, if any, is given when it starts.
    void startQueued(std::int16_t object, const std::string& name, const std::string& bone, int number, bool keep, bool hard, int priority, std::uint64_t expires,
                     const std::string& picture, int firstFrame, const PlaceOf& placeOf);
    // Starts the next of each queue whose front has ended. Call while the simulation runs, with its tick count.
    void advanceQueues(std::uint64_t now, const PlaceOf& placeOf);
    // Forgets the object's queue. What is playing plays on.
    void clearQueue(std::int16_t object);

    // Plays the effect once, on its own, with no way to stop it.
    void startOnce(std::int16_t object, const std::string& name, const std::string& bone, const PlaceOf& placeOf);

    // Stops the effect if it is there. A hard stop forgets it.
    void stop(std::int16_t object, const std::string& name, const std::string& bone, int number, bool hard);

    // Stops every effect of an object started that way (AnyKind: all of them), but those with `keep`. A hard stop
    // forgets them.
    void stopAll(std::int16_t object, Kind kind, bool hard);

    // Effects follow their objects. Call before the player's update.
    void follow(const PlaceOf& placeOf);

    std::size_t recordCount() const;

private:
    struct Record {
        std::shared_ptr<PlayingEffect> effect;
        Kind kind = AnyKind;
        bool keep = false;
    };
    using Key = std::tuple<std::string, std::string, int>;  // name, bone, number
    struct Queued {
        Key key;
        bool keep = false, hard = false;
        int priority = 0;
        std::uint64_t expires = 0;
        std::string picture;
        int firstFrame = 0;
        std::shared_ptr<PlayingEffect> effect;  // once it has started
        bool operator<(const Queued& other) const { return priority < other.priority; }
    };
    Record* find(std::int16_t object, const Key& key);
    bool startInTurn(std::int16_t object, Queued& queued, const PlaceOf& placeOf);

    EffectPlayer& player_;
    std::map<std::int16_t, std::map<Key, Record>> objects_;
    std::map<std::int16_t, std::vector<Queued>> queues_;  // each a heap, the highest priority at the front
};

// Adds the sprites of every playing particle system: their corners, and a batch for each that has an image, by layer.
// `imageOf` gives the image of a block's texture, negative for none.
void appendEffectSprites(const EffectPlayer& player, const ParticleView& view, const std::function<int(const std::string&)>& imageOf, std::vector<ParticleVertex>& vertices,
                         std::vector<render::ParticleBatch>& batches);

} // namespace ts2::scene
