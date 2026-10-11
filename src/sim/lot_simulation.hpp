#pragma once

#include "content/resource_index.hpp"
#include "sim/clock.hpp"
#include "sim/index_code_source.hpp"
#include "sim/lot_world.hpp"
#include "sim/vm.hpp"
#include "sim/world.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>

namespace ts2::sim {

// A saved lot brought to life: the game's object code, scripts and want definitions, and the lot's
// objects with their saved script stacks, run a tick at a time.
class LotSimulation {
public:
    // How long a tick lasts at normal and high speed (ultra runs as fast as it can).
    static constexpr double kNormalTickMs = 1000.0 / 23.83;
    static constexpr double kHighTickMs = kNormalTickMs / 3;

    // Loads the code from the install's object packages and the lot from its package.
    LotSimulation(const std::filesystem::path& install, const std::filesystem::path& lot);

    // Runs one tick: sleeping scripts wait out their ticks and the others run until they sleep, end
    // or stop; then the Sim clock moves on. The world's clock (animation time) moves on a normal tick.
    void tick();

    // The simulator globals (the clock, the speed, the funds...), as saved with the lot at first.
    std::span<std::int16_t> globals() { return vm_.globals; }

    World& world() { return world_; }
    CodeSource& code() { return code_; }
    std::size_t ticks() const { return ticks_; }

    // Why scripts stopped (outcome, reason and detail), how often, and the first time it happened.
    struct Stop {
        std::size_t count = 0;
        RunResult first;
        std::int16_t object = 0;
        std::size_t tick = 0;
    };
    const std::map<std::string, Stop>& stops() const { return stops_; }

    const LotWorldReport& report() const { return report_; }
    // Objects reset when the lot loaded because their type changed since the save.
    std::size_t resets() const { return resets_; }

private:
    content::ResourceIndex index_;
    IndexCodeSource code_;
    World world_;
    Vm vm_;
    LotWorldReport report_;
    std::size_t ticks_ = 0;
    std::map<std::string, Stop> stops_;
    std::size_t resets_ = 0;

    void startObjects();
    void note(const RunResult& r, std::int16_t object);
};

} // namespace ts2::sim
