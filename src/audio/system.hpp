#pragma once

// Plays the install's sounds through the system's audio device, by the game's sound manager rules
// (see manager.hpp). One per game, owned by the app and driven from its main loop.

#include "audio/mix.hpp"
#include "audio/names.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ts2::audio {

// A sound event of the simulation: a Play Sound primitive or an animation's sound mark.
struct Event {
    std::string name;
    std::int16_t object = 0;
    bool stop = false;
    NameInputs inputs;
    std::optional<Vec3> position;  // the object's place, none for no place
};

class System {
public:
    System();
    ~System();
    System(const System&) = delete;
    System& operator=(const System&) = delete;

    // Indexes the install's sound packages on a low priority thread. Only the first call does
    // anything. Sounds asked for before it finishes are dropped.
    void addInstall(const std::filesystem::path& installRoot);
    [[nodiscard]] bool ready() const;

    // The sound quality option's AudioPerformance (0 low, 1 medium, 2 high), which sets the voice limits.
    void setPerformanceLevel(int level);

    void play(const Event& event);
    // Stops every sound of one object, as Stop All Sounds does.
    void stopObject(std::int16_t object);
    void stopAll();
    // The game's pause and the simulator's speed index (2 normal, 3 fast, 4 fastest).
    void setPaused(bool paused);
    void setSpeed(int index);
    // The camera the listener follows, in the same frame as the sounds' positions.
    void setListener(const Vec3& eye, const Vec3& target, const Vec3& up);
    // Moves each playing object sound to where `positionOf` says its object is now. Objects it
    // has no place for keep their last.
    void updatePositions(const std::function<std::optional<Vec3>(std::int16_t object)>& positionOf);
    // Once a frame: applies the rules, hands decoded samples to the mixer and drops ended sounds.
    void update();

    // A UI sound by name, or by the track id a UI script names (btnclicksnd). Its id, 0 when it
    // does not play.
    std::uint32_t playUi(std::string_view name);
    std::uint32_t playUiId(std::uint32_t id);
    void stop(std::uint32_t id);

    // Plays a track by name, waiting for indexing first and for the sound to end, for
    // --play-sound. Its length in seconds, 0 when nothing played.
    double playName(std::string_view name);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ts2::audio
