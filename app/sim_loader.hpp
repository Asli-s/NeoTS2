#pragma once

#include "scene/scene.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ts2::scene {
class Assets;
class SceneBuilder;
class RigLibrary;
} // namespace ts2::scene

namespace app {

// Builds Sims in the background, one at a time and in the order asked, as the game loads a Sim's
// character before it makes the Sim (Generic Sims Call 0x1D). A load reads the Sim's look from its
// character file and builds its meshes and textures with an asset reader of its own, made as the
// loader starts. A load takes at least as long as the game's does. The lot scene takes the result in
// when the Sim appears (see LotLife::addLoadedSim).
class SimLoader {
public:
    // A Sim built by the loader. Its numbers are the loader's own: the actor's skinned and held
    // instances index `instances`, which name the loader's meshes, whose primitives name its
    // materials, which name its images and cubes. Each mesh, material, image and cube comes once,
    // with the first Sim that uses it, so the taker keeps them for the Sims after.
    struct Built {
        std::filesystem::path file;
        ts2::scene::Actor actor;
        std::vector<ts2::scene::Instance> instances;
        std::vector<std::pair<int, ts2::scene::Mesh>> meshes;
        std::vector<std::pair<int, ts2::scene::Material>> materials;
        std::vector<std::pair<int, ts2::scene::Image>> images;
        std::vector<std::pair<int, std::vector<ts2::scene::Image>>> envCubes;
        std::vector<std::string> slotBones, targetBones;  // see simSlotBones
        std::vector<std::string> problems;
        bool drawn = false;  // it has something to draw
        // When the game would have it loaded: before then load() says it isn't, though a Sim made
        // from it can take it (see kGameLoadTime).
        std::chrono::steady_clock::time_point readyAt;
    };

    explicit SimLoader(std::filesystem::path install);
    ~SimLoader();
    SimLoader(const SimLoader&) = delete;
    SimLoader& operator=(const SimLoader&) = delete;

    // Whether the Sim of `neighbor` is built from `file`, standing in `lotFrame`, and as long ago as
    // the game takes to load one. A first ask starts its load.
    bool load(std::int16_t neighbor, const std::filesystem::path& file, const ts2::scene::Transform& lotFrame);
    // Whether a build of `neighbor` is waiting or running, and no Sim made from it waits to be drawn.
    bool loading(std::int16_t neighbor) const;
    // Gives the built Sim of `neighbor` to the Sim just made from it. As in the game, making a Sim
    // takes its character out of the loaded ones, so the next ask for it loads it again.
    void claim(std::int16_t neighbor);
    // The built Sim of `neighbor` from `file`, taken out (one claimed first). None when there is none.
    std::optional<Built> take(std::int16_t neighbor, const std::filesystem::path& file);

private:
    struct Job {
        std::int16_t neighbor = 0;
        std::filesystem::path file;
        ts2::scene::Transform lotFrame;
    };
    void run();
    Built build(const Job& job);
    // Moves what the builder made for the instances from `first` on into `built`.
    void collect(Built& built, std::size_t first);
    // Each mesh, material, image and cube is sent only once, so those of a Sim that is dropped
    // untaken go with the next Sim taken.
    void keepShared(Built&& dropped);
    void addShared(Built& built);

    const std::filesystem::path install_;
    // The worker's own, made and used on its thread only.
    std::unique_ptr<ts2::scene::Assets> assets_;
    std::unique_ptr<ts2::scene::SceneBuilder> builder_;
    std::unique_ptr<ts2::scene::RigLibrary> rigs_;
    std::set<int> sentMeshes_, sentMaterials_, sentImages_, sentCubes_;
    std::chrono::steady_clock::time_point lastReady_;  // when the last load would be done in the game

    mutable std::mutex mutex_;  // guards what follows
    std::condition_variable wake_;
    std::deque<Job> queue_;
    std::set<std::int16_t> busy_;  // waiting or running
    std::map<std::int16_t, Built> done_;
    std::map<std::int16_t, Built> claimed_;  // given to a Sim made from it, not drawn yet
    std::deque<std::int16_t> doneOrder_, claimedOrder_;  // oldest first
    Built shared_;  // see keepShared
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace app
