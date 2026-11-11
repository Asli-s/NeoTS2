// High lot imposters (the Lot Imposters option's High, not in the original game): every lot's
// imposter pictures drawn again from the lot, sharper, on the layout the game gave them, while a
// neighborhood is open.
#pragma once

#include "formats/lot_description.hpp"
#include "render/irenderer.hpp"
#include "scene/assets.hpp"
#include "scene/imposter_bake.hpp"
#include "scene/imposter_cache.hpp"
#include "scene/scene.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <vector>

namespace app {

// Bakes lots' High pictures one lot at a time: a lot is built and lit on another thread, its shots
// are drawn a batch between frames, and its pictures are finished, kept in the cache folder and
// handed back on that thread again. With `high` off it hands back the lots' imposters with the
// game's own pictures instead. With `poolStylePonds` the pictures show the lots' ponds drawn like
// pools, as lot view draws them with Water Realistic (see scene::addNeighborhoodPonds).
class ImposterBaker {
public:
    struct Lot {
        ts2::nhood::LotDescription lot;
        std::filesystem::path file;
    };
    // A lot's imposter with its High pictures, as addImposter makes it into a terrain of its own, and
    // its textures uploaded ahead (see IRenderer::prepareImposterTextures).
    struct Baked {
        std::uint32_t lot = 0;
        ts2::scene::PaintedTerrain imposter;
        ts2::render::IRenderer::ImposterTextures textures;
    };

    // Nothing is baked by a renderer that can't upload on another thread.
    ImposterBaker(std::filesystem::path install, ts2::render::IRenderer& renderer, std::filesystem::path cacheFolder, std::vector<Lot> lots,
                  float zOffset, bool high = true, bool poolStylePonds = false);
    ~ImposterBaker();
    ImposterBaker(const ImposterBaker&) = delete;
    ImposterBaker& operator=(const ImposterBaker&) = delete;

    // Moves the baking on: takes the pictures the GPU has drawn since, and starts drawing more shots
    // for about `budget` of the drawing thread's time (at least one). Called between frames on the
    // drawing thread, which never waits for the GPU here. Returns the lots whose pictures are done.
    std::vector<Baked> step(std::chrono::nanoseconds budget);
    bool done() const;

    // A step's share of a frame: each shot takes the drawing thread a millisecond or two, and each
    // texture's smaller levels a fraction of one.
    static constexpr std::chrono::nanoseconds kStepBudget = std::chrono::milliseconds(3);
    static constexpr std::size_t kStepLevels = 16;

private:
    // A lot being baked: its shots and which instances each layer shows, its scene on the GPU, and
    // its pages being filled.
    struct Job {
        Lot lot;
        ts2::scene::ImposterCacheKey key;
        std::vector<ts2::scene::ImposterShot> shots;
        std::vector<std::uint8_t> layers[4];  // by ImposterShot::Layer
        // The lot as seen with its walls up, whose pool water lights the pools' floors and walls with
        // its caustics in every picture, as the game's bake draws them.
        std::vector<std::uint8_t> pools;
        ts2::render::IRenderer::Prepared gpu;
        ts2::scene::Image groundLightMap;
        int lotWidth = 0, lotDepth = 0;
        // With ponds drawn like pools, the tiles under their water, wetSide a row, for their bed.
        std::vector<std::uint8_t> wet;
        int wetSide = 0;
        bool isWet(int x, int y) const {
            return x >= 0 && y >= 0 && x < wetSide && y < wetSide && wet[static_cast<std::size_t>(y) * static_cast<std::size_t>(wetSide) + static_cast<std::size_t>(x)] != 0;
        }
        std::size_t next = 0;  // the next shot to draw
        ts2::scene::ImposterPages pages{ts2::scene::kHighImposterSide};
        std::optional<Baked> cached;  // when its pictures were already kept
        // The shots the GPU is drawing, oldest first, and the pages being filled from the ones drawn
        // before (on another thread, a batch at a time). Last, so that it is waited for before the
        // pages go.
        struct Drawing {
            ts2::render::IRenderer::Pictures pictures;
            std::size_t shot = 0;
        };
        std::deque<Drawing> pictures;
        std::future<void> putting;
    };
    std::unique_ptr<Job> build(const Lot& lot);
    Baked finish(std::unique_ptr<Job> job);
    void takeFinished(std::vector<Baked>& out);
    void takeBuilt(std::vector<Baked>& out);
    void putDrawnShots(Job& job);
    // Starts the next shots within the step's budget. False once the renderer keeps failing to.
    bool startShots(Job& job, std::chrono::steady_clock::time_point started, std::chrono::nanoseconds budget);

    std::filesystem::path install_;
    ts2::render::IRenderer& renderer_;
    std::filesystem::path cacheFolder_;
    float zOffset_ = 0;
    bool high_ = true;
    bool poolStylePonds_ = false;
    std::deque<Lot> waiting_;
    std::unique_ptr<ts2::scene::Assets> assets_;  // used on the worker thread only
    std::future<std::unique_ptr<Job>> building_;
    std::unique_ptr<Job> drawing_;
    std::future<Baked> finishing_;
    int failedStarts_ = 0;  // steps in a row the renderer could not start a picture
};

} // namespace app
