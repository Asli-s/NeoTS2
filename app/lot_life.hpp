#pragma once

#include "scene/animation_library.hpp"
#include "scene/scene.hpp"
#include "sim/clock.hpp"
#include "sim/lot_simulation.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace app {

// A lot's simulation, or null (saying why) when it can't be built.
std::unique_ptr<ts2::sim::LotSimulation> loadSimulation(const std::filesystem::path& install, const std::filesystem::path& lot);

// A lot's Sims driven by their own scripts: the lot's simulation, run in step with real time, and
// each Sim's actor playing the animation its scripts started last (its idle loop until then).
class LotLife {
public:
    LotLife(std::unique_ptr<ts2::sim::LotSimulation> sim, ts2::scene::Assets& assets);

    // Runs the simulation for `ms` of real time at its speed (whole ticks; the rest waits for the
    // next call; nothing while paused), then has each Sim's actor play what its scripts animate.
    void update(double ms, ts2::scene::Scene& scene);

    // The game speed and pause, kept in the simulator's globals (scripts change them too).
    ts2::sim::Speed speed();
    void setSpeed(ts2::sim::Speed speed);
    bool paused();
    void setPaused(bool paused);

    // The Sim clock as the live panel shows it, and the household's funds.
    std::string clockText() { return ts2::sim::clockText(sim_->globals()); }
    std::int32_t funds() { return ts2::sim::funds(sim_->globals()); }
    // The selected Sim's Aspiration person data (0 when no Sim is selected).
    std::int16_t aspiration();

    // The simulation's clock in seconds: the time the actors are posed at.
    double seconds() const { return sim_->world().clockMs() / 1000.0; }

    ts2::sim::LotSimulation& simulation() { return *sim_; }

private:
    std::unique_ptr<ts2::sim::LotSimulation> sim_;
    ts2::scene::AnimationLibrary animations_;
    double pendingMs_ = 0;
};

} // namespace app
