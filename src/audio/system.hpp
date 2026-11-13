#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ts2::audio {

// What a sound event knows about where it plays: the object's quality and surfaces, and a Sim's
// age, sex and voice for the vox suffix. Missing values are simply not tried.
struct Context {
    bool person = false;
    std::int16_t age = 0;
    std::int16_t gender = 0;
    std::int16_t voice = 0;
    std::int16_t ghost = 0;
    std::string quality;
    std::string destSurface;
    std::string sourceSurface;
    std::string shoe;
};

// Plays the install's sounds through the system's audio device. One per game, owned by the app.
class System {
public:
    System();
    ~System();
    System(const System&) = delete;
    System& operator=(const System&) = delete;

    // Indexes the install's sound packages. Adding the same install again does nothing.
    void addInstall(const std::filesystem::path& installRoot);

    // Plays a sound event. With `stop`, stops the voices that event started on that object instead.
    // `position` (lot coordinates, z up) makes it positional; without one it plays centred.
    void play(std::string_view name, std::int16_t source, bool stop, const Context& context,
              std::optional<std::array<float, 3>> position = std::nullopt);
    // Stops everything that is playing.
    void stopAll();
    // Stops the voices of one object, as Stop All Sounds asks.
    void stopSource(std::int16_t source);
    // Pauses loops with the simulation; one-shots finish.
    void setPaused(bool paused);
    // Drops voices whose object is gone; `alive` says whether an id is still on the lot.
    void pruneSources(const std::function<bool(std::int16_t)>& alive);
    // Moves voices to their objects' current positions. `positionOf` returns none for sources that
    // do not move (static objects), which keep the position they started with.
    void updatePositions(const std::function<std::optional<std::array<float, 3>>(std::int16_t source)>& positionOf);
    // Where the listener is (the camera's target) and its right vector, for distance and pan.
    void setListener(std::array<float, 3> at, std::array<float, 3> right);

    // Plays a track by the name the game files it under, without modifiers, and returns its
    // length in seconds (0 when there is no such track). For --play-sound.
    [[nodiscard]] double playName(std::string_view name);
    // Plays a UI sound by name, centred, without the per-click log. False before indexing finishes
    // or when the name does not resolve.
    bool playUi(std::string_view name);
    // The track name an event resolves to, empty when none exists.
    [[nodiscard]] std::string resolve(std::string_view name, const Context& context);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ts2::audio
