#include "sim_loader.hpp"

#include "diag/log.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/sim_builder.hpp"
#include "scene/sims.hpp"

#include <chrono>
#include <algorithm>
#include <format>
#include <iterator>
#include <stdexcept>

namespace app {

namespace {
// Built Sims whose Sim never appeared are dropped past this many, oldest first.
constexpr std::size_t kMaxDone = 16;
// The game takes about this long to load a Sim's character, and scripts time things by it: a car
// waits for its driver, so a second car sent with the first comes once the first has driven on. A
// Sim scripts make before then (their wait ran out, as at ultra speed) is drawn as soon as it is built.
constexpr std::chrono::milliseconds kGameLoadTime{1300};

double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
} // namespace

SimLoader::SimLoader(std::filesystem::path install) : install_(std::move(install)), thread_([this] { run(); }) {}

SimLoader::~SimLoader() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

bool SimLoader::load(std::int16_t neighbor, const std::filesystem::path& file, const ts2::scene::Transform& lotFrame) {
    {
        std::lock_guard lock(mutex_);
        if (const auto it = done_.find(neighbor); it != done_.end() && it->second.file == file) {
            return std::chrono::steady_clock::now() >= it->second.readyAt;
        }
        if (busy_.contains(neighbor)) {
            return false;
        }
        busy_.insert(neighbor);
        queue_.push_back({neighbor, file, lotFrame});
    }
    wake_.notify_all();
    return false;
}

bool SimLoader::loading(std::int16_t neighbor) const {
    std::lock_guard lock(mutex_);
    return busy_.contains(neighbor) && !claimed_.contains(neighbor);
}

void SimLoader::claim(std::int16_t neighbor) {
    std::lock_guard lock(mutex_);
    const auto it = done_.find(neighbor);
    if (it == done_.end()) {
        return;
    }
    if (const auto old = claimed_.find(neighbor); old != claimed_.end()) {
        keepShared(std::move(old->second));
    }
    claimed_[neighbor] = std::move(it->second);
    done_.erase(it);
    std::erase(doneOrder_, neighbor);
    std::erase(claimedOrder_, neighbor);
    claimedOrder_.push_back(neighbor);
    if (claimedOrder_.size() > kMaxDone) {
        keepShared(std::move(claimed_.at(claimedOrder_.front())));
        claimed_.erase(claimedOrder_.front());
        claimedOrder_.pop_front();
    }
}

std::optional<SimLoader::Built> SimLoader::take(std::int16_t neighbor, const std::filesystem::path& file) {
    std::lock_guard lock(mutex_);
    if (const auto claimed = claimed_.find(neighbor); claimed != claimed_.end() && claimed->second.file == file) {
        Built built = std::move(claimed->second);
        claimed_.erase(claimed);
        std::erase(claimedOrder_, neighbor);
        addShared(built);
        return built;
    }
    const auto it = done_.find(neighbor);
    if (it == done_.end() || it->second.file != file) {
        return std::nullopt;
    }
    Built built = std::move(it->second);
    done_.erase(it);
    std::erase(doneOrder_, neighbor);
    addShared(built);
    return built;
}

void SimLoader::keepShared(Built&& dropped) {
    std::ranges::move(dropped.meshes, std::back_inserter(shared_.meshes));
    std::ranges::move(dropped.materials, std::back_inserter(shared_.materials));
    std::ranges::move(dropped.images, std::back_inserter(shared_.images));
    std::ranges::move(dropped.envCubes, std::back_inserter(shared_.envCubes));
}

void SimLoader::addShared(Built& built) {
    if (!built.drawn) {
        return;
    }
    std::ranges::move(shared_.meshes, std::back_inserter(built.meshes));
    std::ranges::move(shared_.materials, std::back_inserter(built.materials));
    std::ranges::move(shared_.images, std::back_inserter(built.images));
    std::ranges::move(shared_.envCubes, std::back_inserter(built.envCubes));
    shared_ = {};
}

void SimLoader::run() {
    // The asset reader is made at once, so that the first Sim asked for takes no longer than the rest.
    const auto start = std::chrono::steady_clock::now();
    try {
        assets_ = std::make_unique<ts2::scene::Assets>(install_);
        builder_ = std::make_unique<ts2::scene::SceneBuilder>(*assets_);
        rigs_ = std::make_unique<ts2::scene::RigLibrary>(*assets_);
        ts2::diag::info(std::format("background Sim loading ready after {:.1f} s", msSince(start) / 1000));
    } catch (const std::exception& ex) {
        ts2::diag::warning(std::string("background Sim loading can't read the game's files: ") + ex.what());
    }
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        Built built;
        const auto started = std::chrono::steady_clock::now();
        try {
            built = build(job);
            ts2::diag::info(std::format("neighbor {} built in the background in {:.0f} ms", job.neighbor, msSince(started)));
        } catch (const std::exception& ex) {
            built = Built{};
            built.file = job.file;
            built.problems.push_back(std::string("its look can't be read: ") + ex.what());
        }
        // The game loads one at a time, so a load asked for while another was loading is ready that
        // long after the one before it.
        built.readyAt = std::max(started, lastReady_) + kGameLoadTime;
        lastReady_ = built.readyAt;
        std::unique_lock lock(mutex_);
        busy_.erase(job.neighbor);
        std::erase(doneOrder_, job.neighbor);
        if (const auto old = done_.find(job.neighbor); old != done_.end()) {
            keepShared(std::move(old->second));
        }
        done_[job.neighbor] = std::move(built);
        doneOrder_.push_back(job.neighbor);
        if (doneOrder_.size() > kMaxDone) {
            keepShared(std::move(done_.at(doneOrder_.front())));
            done_.erase(doneOrder_.front());
            doneOrder_.pop_front();
        }
    }
}

SimLoader::Built SimLoader::build(const Job& job) {
    if (!assets_) {
        throw std::runtime_error("the game's files can't be read");
    }
    Built built;
    built.file = job.file;
    const ts2::scene::Assets::CharacterInUse inUse(*assets_, job.file);
    const ts2::scene::SimLook look = ts2::scene::simLook(*assets_, job.file);
    built.problems = look.problems;
    if (look.parts.empty()) {
        return built;
    }
    auto& scene = builder_->scene();
    const std::size_t first = scene.instances.size();
    std::vector<ts2::scene::Transform> pose;
    try {
        built.actor = ts2::scene::buildSim(*builder_, *rigs_, look, job.lotFrame, 0, std::to_string(job.neighbor),
                                           ts2::scene::standingIdle(*assets_, look.age), pose);
    } catch (...) {
        scene.instances.resize(first);
        throw;
    }
    built.actor.basePose = ts2::scene::standingPose(*assets_, look.age);
    built.slotBones = ts2::scene::simSlotBones(*assets_, job.file);
    built.targetBones = ts2::scene::simSlotBones(*assets_, job.file, true);
    collect(built, first);
    built.drawn = true;
    return built;
}

void SimLoader::collect(Built& built, std::size_t first) {
    auto& scene = builder_->scene();
    std::map<std::size_t, std::size_t> moved;
    for (std::size_t i = first; i < scene.instances.size(); ++i) {
        moved[i] = built.instances.size();
        built.instances.push_back(std::move(scene.instances[i]));
    }
    // The builder keeps its meshes and materials for the next Sim wearing them.
    scene.instances.resize(first);
    for (auto& s : built.actor.skinned) {
        s.instance = moved.at(s.instance);
    }
    for (auto& h : built.actor.held) {
        h.instance = moved.at(h.instance);
    }
    const auto sendImage = [&](int index) {
        if (index >= 0 && static_cast<std::size_t>(index) < scene.images.size() && sentImages_.insert(index).second) {
            // The builder only needs an image's place in its list once it is made.
            built.images.emplace_back(index, std::move(scene.images[static_cast<std::size_t>(index)]));
        }
    };
    for (const auto& instance : built.instances) {
        const int m = instance.mesh;
        if (m < 0 || static_cast<std::size_t>(m) >= scene.meshes.size() || !sentMeshes_.insert(m).second) {
            continue;
        }
        const ts2::scene::Mesh& mesh = scene.meshes[static_cast<std::size_t>(m)];
        for (const auto& p : mesh.primitives) {
            if (p.material < 0 || static_cast<std::size_t>(p.material) >= scene.materials.size() || !sentMaterials_.insert(p.material).second) {
                continue;
            }
            const ts2::scene::Material& material = scene.materials[static_cast<std::size_t>(p.material)];
            sendImage(material.image);
            sendImage(material.normalMap);
            sendImage(material.mask);
            sendImage(material.detail);
            const int cube = material.envCube;
            if (cube >= 0 && static_cast<std::size_t>(cube) < scene.envCubes.size() && sentCubes_.insert(cube).second) {
                built.envCubes.emplace_back(cube, std::move(scene.envCubes[static_cast<std::size_t>(cube)]));
            }
            built.materials.emplace_back(p.material, material);
        }
        built.meshes.emplace_back(m, mesh);
    }
}

} // namespace app
