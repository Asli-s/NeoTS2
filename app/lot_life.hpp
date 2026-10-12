#pragma once

#include "scene/animation_library.hpp"
#include "scene/scene.hpp"
#include "sim/clock.hpp"
#include "formats/tga.hpp"
#include "sim/lot_simulation.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
    // The selected Sim's object id (0: none), and picking another Sim.
    std::int16_t selected();
    void select(std::int16_t sim);
    // The household's Sims (object ids, in family order), and a Sim's portrait for its life stage
    // from its character file.
    const std::vector<std::int16_t>& household() const { return sim_->household(); }
    std::optional<ts2::tga::Pixels> portrait(std::int16_t sim);
    // A Sim's neighbor id (0 when it has none), and where a neighbor's portrait for its life stage
    // is: its character file and the picture's instance there (nullopt when unknown).
    std::int16_t neighborId(std::int16_t sim);
    struct PortraitFile {
        std::filesystem::path file;
        std::uint32_t instance = 0;
    };
    std::optional<PortraitFile> neighborPortrait(std::int16_t neighbor);
    // A Sim's life stage (person data 0x3A), its motives as the panels show them (whole numbers)
    // and its mood (motive 3); 0 for a Sim that is gone.
    std::int16_t lifeStage(std::int16_t sim);
    std::array<int, 16> motives(std::int16_t sim);
    int mood(std::int16_t sim);
    // A Sim's current wants and fears, and what a want is (null for an unknown one).
    std::vector<ts2::swaf::Want> wants(std::int16_t sim);
    std::vector<ts2::swaf::Want> fears(std::int16_t sim);
    const ts2::content::WantDefinition* wantDefinition(std::uint32_t guid) const { return sim_->world().wantDefinition(guid); }
    // A Sim's aspiration score (person data 0xA0); 0 for a Sim that is gone.
    std::int16_t aspirationScore(std::int16_t sim);

    // The simulation's clock in seconds: the time the actors are posed at.
    double seconds() const { return sim_->world().clockMs() / 1000.0; }

    ts2::sim::LotSimulation& simulation() { return *sim_; }
    ts2::scene::AnimationLibrary& animations() { return animations_; }

private:
    std::unique_ptr<ts2::sim::LotSimulation> sim_;
    ts2::scene::AnimationLibrary animations_;
    double pendingMs_ = 0;
};

} // namespace app
