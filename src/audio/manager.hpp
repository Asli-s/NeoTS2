#pragma once

// The game's sound manager rules without any audio device: which requests start, which voices are
// stolen for them, what pauses them and how loud each is and in which ear. The audio system feeds
// it requests and the listener and plays what it says.

#include "audio/mix.hpp"
#include "audio/names.hpp"
#include "audio/track.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ts2::audio {

// A request to start a sound.
struct Request {
    SoundKey track;       // the resolved track's key
    SoundKey event;       // the event's own name's key, which a stop names
    std::int16_t object = 0;  // the object it plays on, 0 for none
    Group group = Group::Sfx;
    std::optional<Vec3> position;  // where its object is, none for no place
};

// Why a request did not start, for the log.
enum class Dropped { None, VoiceAtSpeed, AlreadyPlaying, Chance, GroupPaused, NoVoice, NoSample };

class Manager {
public:
    struct Instance {
        std::uint32_t id = 0;
        Request request;
        TrackSettings settings;
        SampleKey sample;
        double startMs = 0;
        double lengthMs = 0;  // 0 until its sample is decoded
        double playedMs = 0;
        bool gamePaused = false;
        bool outOfRange = false;
        float fade = 1;        // the pause fade, 1 heard, 0 silent
        float primaryGain = 1;  // the distance drop to the listener, for the steal rule
        bool stopped = false;   // to be removed
    };

    // What a voice plays with this update.
    struct Output {
        std::uint32_t id = 0;
        EarGains mono;                       // for a one-channel sample
        std::array<EarGains, 2> stereo{};    // for a two-channel sample, by source channel
        float pitch = 1;
        bool advancing = true;  // false while paused and faded out
    };

    explicit Manager(std::uint32_t seed = 1) : random_(seed) {}

    void setTuning(const Tuning& tuning) { tuning_ = tuning; }
    // The sound quality option's AudioPerformance, 0 to 2.
    void setPerformanceLevel(int level);
    // The simulator's speed index: 2 normal, 3 fast, 4 fastest.
    void setSpeedIndex(int index) { speedIndex_ = index; }
    // Pauses or resumes every sound but music, UI and cinematic ones, with each track's fade-out
    // time. While paused, new one-shots outside the stereo, music, TV and stitcher groups are dropped
    // and new others start paused.
    void setGamePaused(bool paused);
    void setListener(const Listener& listener) { listener_ = listener; }

    // Starts a sound, as the game's start rules allow: its id, or none with the reason it was
    // dropped. `samples` is the track's hit list, one of them is picked at random.
    std::optional<std::uint32_t> start(const Request& request, const TrackSettings& settings, std::span<const SampleKey> samples,
                                       double nowMs, Dropped* why = nullptr);
    // Stops the sounds an event started on an object (0 for sounds with no object).
    void stopEvent(SoundKey event, std::int16_t object);
    // Stops every sound of one object, as Stop All Sounds does.
    void stopObject(std::int16_t object);
    void stop(std::uint32_t id);
    void stopAll();

    // A decoded sample's length, and how far a voice has played, from the mixer.
    void setLength(std::uint32_t id, double lengthMs);
    void setPlayed(std::uint32_t id, double playedMs);
    void moveObject(std::int16_t object, const Vec3& position);

    // Applies distance, range and pause fades for `elapsedMs` since the last update, and returns
    // what each voice plays with. Stopped sounds are dropped first.
    std::vector<Output> update(double elapsedMs);

    [[nodiscard]] const std::vector<Instance>& instances() const { return instances_; }
    [[nodiscard]] const Instance* find(std::uint32_t id) const;

private:
    [[nodiscard]] bool groupPaused(Group group) const;
    [[nodiscard]] bool playsFor(const Instance& instance, std::int16_t object) const;
    [[nodiscard]] float distanceToListener(const Instance& instance) const;
    [[nodiscard]] float gainAt(const Request& request, const TrackSettings& settings) const;
    // Makes room for a new sound of `group` with that priority: false when the limit leaves none.
    bool makeRoom(Group group, float priority);
    [[nodiscard]] Output output(Instance& instance, double elapsedMs);

    std::vector<Instance> instances_;
    Tuning tuning_;
    Listener listener_;
    AudioRandom random_;
    int level_ = 1;
    int speedIndex_ = 2;
    bool gamePaused_ = false;
    std::uint32_t nextId_ = 1;
};

} // namespace ts2::audio
