#include "lot_life.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>

namespace app {

namespace {

// After a long pause (a slow frame, a window drag) the simulation catches up at most this many ticks.
constexpr int kMaxTicksPerUpdate = 10;
// At ultra speed the simulation runs as many ticks as fit in this much time each update.
constexpr double kUltraBudgetMs = 8.0;

} // namespace

std::unique_ptr<ts2::sim::LotSimulation> loadSimulation(const std::filesystem::path& install, const std::filesystem::path& lot) {
    try {
        return std::make_unique<ts2::sim::LotSimulation>(install, lot);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "the lot's Sims won't come to life: %s\n", ex.what());
        return nullptr;
    }
}

LotLife::LotLife(std::unique_ptr<ts2::sim::LotSimulation> sim, ts2::scene::Assets& assets)
    : sim_(std::move(sim)), animations_(assets) {
    sim_->world().setAnimationLengths([this](const std::string& name) { return animations_.length(name); });
}

ts2::sim::Speed LotLife::speed() {
    const auto s = static_cast<ts2::sim::Speed>(sim_->globals()[ts2::sim::global::kSpeed]);
    return s == ts2::sim::Speed::High || s == ts2::sim::Speed::Ultra ? s : ts2::sim::Speed::Normal;
}

void LotLife::setSpeed(ts2::sim::Speed speed) {
    sim_->globals()[ts2::sim::global::kSpeed] = static_cast<std::int16_t>(speed);
    setPaused(false);
}

std::int16_t LotLife::aspiration() {
    constexpr std::size_t kAspiration = 46;
    const auto* sim = sim_->world().find(sim_->globals()[ts2::sim::global::kSelectedPerson]);
    return sim ? sim->personData[kAspiration] : 0;
}

bool LotLife::paused() { return sim_->globals()[ts2::sim::global::kPaused] != 0; }

void LotLife::setPaused(bool paused) { sim_->globals()[ts2::sim::global::kPaused] = paused ? 1 : 0; }

void LotLife::update(double ms, ts2::scene::Scene& scene) {
    using ts2::sim::LotSimulation;
    using ts2::sim::Speed;
    if (paused()) {
        pendingMs_ = 0;
    } else if (speed() == Speed::Ultra) {
        const auto start = std::chrono::steady_clock::now();
        do {
            sim_->tick();
        } while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() < kUltraBudgetMs);
        pendingMs_ = 0;
    } else {
        const double tickMs = speed() == Speed::High ? LotSimulation::kHighTickMs : LotSimulation::kNormalTickMs;
        pendingMs_ = std::min(pendingMs_ + ms, kMaxTicksPerUpdate * tickMs);
        while (pendingMs_ >= tickMs) {
            sim_->tick();
            pendingMs_ -= tickMs;
        }
    }
    for (auto& actor : scene.actors) {
        const auto* object = actor.object ? sim_->world().find(static_cast<std::int16_t>(actor.object)) : nullptr;
        if (object == nullptr || !object->animation) {
            continue;
        }
        // A newly started animation (by its start time): play it once from then.
        const auto& playing = *object->animation;
        const double start = playing.startMs / 1000.0;
        if (actor.loop || actor.start != start) {
            if (auto bound = animations_.bound(actor.rig, playing.name)) {
                actor.animation = std::move(bound);
                actor.start = start;
                actor.speed = playing.speed;
                actor.loop = false;
            }
        }
    }
}

} // namespace app
