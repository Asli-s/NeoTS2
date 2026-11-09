#include "imposter_baker.hpp"

#include "lot_light.hpp"

#include "content/lot.hpp"
#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "scene/assets.hpp"
#include "scene/builder.hpp"
#include "scene/imposters.hpp"
#include "scene/lot_scene.hpp"
#include "scene/view.hpp"

#include <chrono>
#include <format>
#include <utility>

namespace app {

namespace {

using namespace ts2;

// The game's lighting state for drawing imposters from lot houses (Lighting.txt).
constexpr const char* kImposterLighting = "neighbourhoodImposter";

template <typename T>
bool ready(const std::future<T>& job) {
    return job.valid() && job.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

render::Camera cameraOf(const scene::ImposterShot& shot) {
    render::Camera camera;
    camera.eye = {shot.eye[0], shot.eye[1], shot.eye[2]};
    camera.target = {shot.target[0], shot.target[1], shot.target[2]};
    camera.up = {shot.up[0], shot.up[1], shot.up[2]};
    camera.orthographic = shot.orthographic;
    camera.nearPlane = shot.nearPlane;
    camera.farPlane = shot.farPlane;
    return camera;
}

} // namespace

ImposterBaker::ImposterBaker(std::filesystem::path install, render::IRenderer& renderer, std::filesystem::path cacheFolder, std::vector<Lot> lots,
                             float zOffset, bool high)
    : install_(std::move(install)), renderer_(renderer), cacheFolder_(std::move(cacheFolder)), zOffset_(zOffset), high_(high) {
    if (renderer_.capabilities().threadedPrepare || !high_) {
        waiting_.assign(std::make_move_iterator(lots.begin()), std::make_move_iterator(lots.end()));
    }
}

ImposterBaker::~ImposterBaker() {
    // The worker's results hold GPU objects and the worker uses the assets: wait for both.
    if (building_.valid()) {
        building_.wait();
    }
    if (finishing_.valid()) {
        finishing_.wait();
    }
}

bool ImposterBaker::done() const {
    return waiting_.empty() && !building_.valid() && !drawing_ && !finishing_.valid();
}

std::unique_ptr<ImposterBaker::Job> ImposterBaker::build(const Lot& lot) {
    auto job = std::make_unique<Job>();
    job->lot = lot;
    const auto key = scene::imposterCacheKey(lot.file);
    if (!key) {
        return nullptr;
    }
    job->key = *key;
    std::vector<std::string> warnings;
    if (!high_) {
        Baked original{lot.lot.id, {}};
        if (!scene::addImposter(original.imposter, lot.file, lot.lot, zOffset_, warnings)) {
            return nullptr;
        }
        job->cached = std::move(original);
        return job;
    }
    // Pictures already kept for the lot as it is now are used as they are.
    if (auto kept = scene::readImposterCache(scene::imposterCacheFile(cacheFolder_, lot.file), *key)) {
        Baked baked{lot.lot.id, {}};
        if (!scene::addImposter(baked.imposter, lot.file, lot.lot, zOffset_, warnings, nullptr, &*kept)) {
            return nullptr;
        }
        job->cached = std::move(baked);
        return job;
    }
    dbpf::Package package{lot.file};
    const auto imposter = scene::readImposter(package, warnings);
    if (!imposter) {
        return nullptr;
    }
    if (!assets_) {
        assets_ = std::make_unique<scene::Assets>(install_);
    }
    // Lit as the game lights a lot it draws an imposter from, walls up and every storey shown.
    scene::SceneBuilder builder(*assets_);
    const auto saved = content::loadLot(lot.file);
    scene::addLot(builder, saved, lot.file);
    lightLot(*assets_, builder.scene(), saved, lot.file, kImposterLighting);
    scene::Scene& lotScene = builder.scene();
    scene::prepareImposterScene(lotScene);
    scene::View view;
    view.level = scene::topLevel(lotScene);
    view.walls = scene::WallView::Up;
    const auto visible = scene::visibleInstances(lotScene, view);
    job->pools = visible;
    for (int layer = 0; layer < 4; ++layer) {
        auto& shown = job->layers[layer];
        shown.resize(lotScene.instances.size());
        for (std::size_t i = 0; i < shown.size(); ++i) {
            shown[i] = i < visible.size() && visible[i] &&
                       scene::inImposterLayer(lotScene.instances[i], static_cast<scene::ImposterShot::Layer>(layer), lotScene.groundLevel);
        }
    }
    for (const auto& instance : lotScene.instances) {
        if (instance.part == scene::Part::Other && instance.lightMapMultiply && instance.lightMap >= 0 &&
            static_cast<std::size_t>(instance.lightMap) < lotScene.images.size()) {
            job->groundLightMap = lotScene.images[static_cast<std::size_t>(instance.lightMap)];
            break;
        }
    }
    job->lotWidth = static_cast<int>(lotScene.lotSize[0]);
    job->lotDepth = static_cast<int>(lotScene.lotSize[1]);
    job->shots = scene::imposterShots(*imposter, scene::imposterSliceHeights(*imposter));
    job->gpu = renderer_.prepare(lotScene);
    return job;
}

ImposterBaker::Baked ImposterBaker::finish(std::unique_ptr<Job> job) {
    Baked baked{job->lot.lot.id, {}};
    // Without a ground light map the bake has no terrain light map, as the game's drops it when the
    // lot's floor light map is missing.
    const scene::Image* groundLightMap = job->groundLightMap.width ? &job->groundLightMap : nullptr;
    auto pictures = scene::compressedImposterPictures(job->pages.finish(groundLightMap, job->lotWidth, job->lotDepth));
    if (!scene::writeImposterCache(scene::imposterCacheFile(cacheFolder_, job->lot.file), job->key, pictures)) {
        diag::warnOnce("High lot imposters: pictures could not be kept in " + diag::utf8(cacheFolder_));
    }
    std::vector<std::string> warnings;
    scene::addImposter(baked.imposter, job->lot.file, job->lot.lot, zOffset_, warnings, nullptr, &pictures);
    if (renderer_.capabilities().threadedPrepare) {
        baked.textures = renderer_.prepareImposterTextures(baked.imposter.imposterTextures);
    }
    return baked;
}

void ImposterBaker::takeFinished(std::vector<Baked>& out) {
    if (ready(finishing_)) {
        try {
            out.push_back(finishing_.get());
        } catch (const std::exception& ex) {
            diag::warning(std::string("High lot imposters: a lot's pictures could not be finished: ") + ex.what());
        }
    }
}

void ImposterBaker::takeBuilt(std::vector<Baked>& out) {
    if (drawing_ || !ready(building_)) {
        return;
    }
    try {
        if (auto job = building_.get()) {
            if (job->cached) {
                out.push_back(std::move(*job->cached));
            } else {
                drawing_ = std::move(job);
            }
        }
    } catch (const std::exception& ex) {
        diag::warning(std::string("High lot imposters: a lot could not be built: ") + ex.what());
    }
}

// The shots the GPU has drawn go into the pages on another thread, once it is done with the ones
// before, so that the drawing thread never waits for it.
void ImposterBaker::putDrawnShots(Job& job) {
    if (job.putting.valid() && !ready(job.putting)) {
        return;
    }
    // Reading a picture back copies it, so a step takes about a terrain's worth at most.
    constexpr std::size_t kMostPixelsTaken = std::size_t{1} << 22;
    std::size_t taken = 0;
    std::vector<std::size_t> drawn;
    std::vector<scene::Image> pictures;
    while (!job.pictures.empty() && (drawn.empty() || taken < kMostPixelsTaken)) {
        auto picture = renderer_.takePreparedImages(*job.pictures.front().pictures);
        if (!picture) {
            break;
        }
        drawn.push_back(job.pictures.front().shot);
        pictures.push_back(picture->empty() ? scene::Image{} : std::move(picture->front()));
        taken += std::size_t{pictures.back().width} * pictures.back().height;
        job.pictures.pop_front();
    }
    if (drawn.empty()) {
        return;
    }
    if (job.putting.valid()) {
        job.putting.get();
    }
    job.putting = std::async(std::launch::async, [&job, drawn = std::move(drawn), pictures = std::move(pictures)] {
        for (std::size_t i = 0; i < drawn.size(); ++i) {
            job.pages.put(job.shots[drawn[i]], pictures[i], scene::kImposterSupersample);
        }
    });
}

bool ImposterBaker::startShots(Job& job, std::chrono::steady_clock::time_point started, std::chrono::nanoseconds budget) {
    // More in flight than this waits for the GPU to catch up.
    constexpr std::size_t kMostInFlight = 16;
    // A GPU call can fail for a moment, and one that keeps failing means the renderer cannot draw them.
    constexpr int kMostFailedStarts = 60;
    // The scene's textures get their smaller levels a few at a time before the first shot.
    const bool levelsMade = renderer_.makePreparedLevels(job.gpu, kStepLevels);
    bool first = levelsMade;
    while (levelsMade && job.next < job.shots.size() && job.pictures.size() < kMostInFlight &&
           (first || std::chrono::steady_clock::now() - started < budget)) {
        const std::size_t at = job.next++;
        const auto& shot = job.shots[at];
        const auto rect = scene::texelRect(shot, job.pages.side());
        const auto width = static_cast<unsigned>(std::max(0, rect.width())) * scene::kImposterSupersample;
        const auto height = static_cast<unsigned>(std::max(0, rect.height())) * scene::kImposterSupersample;
        if (width == 0 || height == 0) {
            continue;
        }
        first = false;
        auto inFlight = renderer_.startPreparedImages(job.gpu, {{cameraOf(shot), width, height, job.layers[static_cast<int>(shot.layer)], job.pools}});
        if (!inFlight) {
            job.next = at;
            return ++failedStarts_ < kMostFailedStarts;
        }
        failedStarts_ = 0;
        job.pictures.push_back({std::move(inFlight), at});
    }
    return true;
}

std::vector<ImposterBaker::Baked> ImposterBaker::step(std::chrono::nanoseconds budget) {
    const auto started = std::chrono::steady_clock::now();
    std::vector<Baked> out;
    takeFinished(out);
    takeBuilt(out);
    if (drawing_) {
        Job& job = *drawing_;
        putDrawnShots(job);
        if (!startShots(job, started, budget)) {
            diag::warnOnce("High lot imposters: the renderer can't draw them");
            waiting_.clear();
            drawing_.reset();
            return out;
        }
        if (job.next >= job.shots.size() && job.pictures.empty() && !finishing_.valid() && (!job.putting.valid() || ready(job.putting))) {
            if (job.putting.valid()) {
                job.putting.get();
            }
            job.gpu = {};
            finishing_ = std::async(std::launch::async, [this, done = std::move(drawing_)]() mutable { return finish(std::move(done)); });
        }
    }
    // The next lot is built once the last is handed back: its upload on the worker would hold up the
    // drawing thread's upload of the last lot's pictures for as long as it takes.
    if (!building_.valid() && !drawing_ && !finishing_.valid() && out.empty() && !waiting_.empty()) {
        building_ = std::async(std::launch::async, [this, lot = waiting_.front()] { return build(lot); });
        waiting_.pop_front();
    }
    return out;
}

} // namespace app
