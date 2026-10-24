#pragma once

#include "content/neighborhood.hpp"
#include "content/resource_index.hpp"
#include "sim/clock.hpp"
#include "sim/index_code_source.hpp"
#include "sim/lot_world.hpp"
#include "sim/motive_update.hpp"
#include "sim/route_portals.hpp"
#include "sim/route_search.hpp"
#include "sim/route_world.hpp"
#include "sim/vm.hpp"
#include "sim/world.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace ts2::sim {

// A saved lot brought to life: the game's object code, scripts and want definitions, and the lot's
// objects with their saved script stacks, run a tick at a time.
class LotSimulation {
public:
    // How long a tick lasts at normal and high speed (ultra runs as fast as it can).
    static constexpr double kNormalTickMs = 1000.0 / 23.83;
    static constexpr double kHighTickMs = kNormalTickMs / 3;

    // Loads the code from the install's object packages and the lot from its package. `onLoadStep`
    // watches the scripts objects run as the lot loads.
    LotSimulation(const std::filesystem::path& install, const std::filesystem::path& lot,
                  std::function<void(const StepInfo&)> onLoadStep = {});

    // Runs one tick: sleeping scripts wait out their ticks and the others run until they sleep, end
    // or stop. Then the Sim clock moves on. The world's clock (animation time) moves on a normal tick.
    void tick();
    // Tells overlays' event trees of what has happened by `nowMs` on the animation clock (a little past
    // the last tick while animations play on between ticks).
    void tellAnimationEvents(double nowMs);

    // Marks objects as seen on screen now: as the game does for what it draws, each gets the next
    // minute of the day (hour * 60 + minute + 1) in its object data 102, which scripts' "On Screen?"
    // compares with the clock (Sims only start idling on screen).
    void markOnScreen(std::span<const std::int16_t> objects);

    // The simulator globals (the clock, the speed, the funds...), as saved with the lot at first.
    std::span<std::int16_t> globals() { return vm_.globals; }
    // The script machine, for tools that watch it step.
    Vm& vm() { return vm_; }

    World& world() { return world_; }
    CodeSource& code() { return code_; }
    // The installed packs, one bit each as nGameState.IsEPInstalled tests them.
    std::uint32_t installedPacks() const { return installedPacks_; }

    // Where routing reads models and fences (the scene's assets). The route world is built from them
    // the first time it is asked for. Scripts find bone slots in the models from then on.
    void setRouteSources(RouteSources sources);
    // The lot's route grids with every footprint up to date, as the game has them before routing
    // work, null until route sources are set or when the lot has no floors.
    const RouteWorld* routeWorld();
    std::size_t ticks() const { return ticks_; }

    // Why scripts stopped (outcome, reason and detail), how often, and the first time it happened.
    struct Stop {
        std::size_t count = 0;
        RunResult first;
        std::int16_t object = 0;
        std::size_t tick = 0;
    };
    const std::map<std::string, Stop>& stops() const { return stops_; }

    // The household living on the lot: its Sims' object ids, in the order of the family's members.
    const std::vector<std::int16_t>& household() const { return household_; }
    // An image resource of a Sim's character file, by instance, empty when missing.
    std::vector<std::uint8_t> simImage(std::int16_t sim, std::uint32_t instance);
    // A neighbor's character file (by neighbor id), on the lot or not: the one whose object
    // definition has the neighbor's GUID. Nullopt when unknown. Only looks it up, reads nothing.
    [[nodiscard]] std::optional<std::filesystem::path> characterFile(std::int16_t neighbor) const;
    // The group of a neighbor's object type, where a neighbor without a character file keeps its looks.
    [[nodiscard]] std::optional<std::uint32_t> characterGroup(std::int16_t neighbor);

    const LotWorldReport& report() const { return report_; }
    // Objects reset when the lot loaded because their type changed since the save.
    std::size_t resets() const { return resets_; }

private:
    content::ResourceIndex index_;
    // Character files by their Sim's GUID, gathered while the simulation is built.
    std::map<std::uint32_t, std::filesystem::path> characterFiles_;
    IndexCodeSource code_;
    World world_;
    Vm vm_;
    std::uint32_t installedPacks_ = 0;
    MotiveUpdate motiveUpdate_;
    LotWorldReport report_;
    std::size_t ticks_ = 0;
    std::map<std::string, Stop> stops_;
    std::size_t resets_ = 0;
    std::vector<std::int16_t> household_;
    std::unique_ptr<const content::Lot> lot_;  // kept until the route world is built
    std::map<std::uint32_t, std::string> objectModels_;  // the lot's model name of each object id
    std::map<std::uint32_t, std::vector<cobject::SavedJoint>> savedJoints_;  // as the lot saved each object's model posed
    std::optional<RouteSources> routeSources_;
    std::optional<RouteWorld> routeWorld_;
    std::map<int, RouteLinks> routeLinks_;  // by level, rebuilt when the grids change
    RouteRandom routeJitter_{1};             // the search's small step costs run on from plan to plan, as the game's
    std::uint32_t routePlans_ = 0;
    // The last start each Sim logged (object and interaction), so a loop of the same one is logged once.
    std::map<std::int16_t, std::pair<std::int16_t, std::uint16_t>> loggedStarts_;
    std::size_t loggedStartCount_ = 0;

    // The VM and its callbacks point into this object, so it stays where it was made.
    LotSimulation(const LotSimulation&) = delete;
    LotSimulation& operator=(const LotSimulation&) = delete;
    void logInteractionStart(const SimObject& sim, const QueuedInteraction& started);

    void startObjects();
    void restartSim(SimObject& sim, const ObjectType& type);
    void resetAfterFailure(SimObject& object, const ObjectType& type);
    void forgetRemoved();
    void forget(SimObject& sim, std::int16_t removed);
    void stepObject(std::int16_t id);
    void runTimers(std::int16_t id);
    void findHousehold(const content::Neighborhood& hood, const std::filesystem::path& lot);
    RouteIntersects intersectionsFor(const SimObject& sim);
    std::pair<bool, float> stairCheck(const SimObject& sim, const StairPortal& stair);
    void findCharacterFiles();
    void note(const RunResult& r, std::int16_t object);
    std::vector<std::pair<float, float>> placementSpots(const SimObject& placed, const SimObject& owner, const slot::Entry& slot,
                                                        std::span<const std::optional<ModelBone>> joints);
    bool objectFits(const SimObject& placed, float x, float y, std::int32_t level, std::int16_t direction);
    std::int16_t stepBlocker(const SimObject& sim, float x, float y, std::int32_t level);
    std::optional<std::int16_t> placeTest(const SimObject& sim, float x, float y, std::int32_t level, const RouteIntersects& intersects);
    bool objectInTheWay(const SimObject& placed, const ObjectType& type, float x, float y, std::int32_t level, std::int16_t direction);
    SlotRoute routeToSlot(const SimObject& sim, const SimObject& owner, const slot::Entry& slot,
                          const std::vector<std::optional<ModelBone>>& joints);
};

} // namespace ts2::sim
