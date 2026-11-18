#include "game.hpp"

#include "content/install.hpp"
#include "diag/log.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>

namespace app {

// The station list is read once, on the first lot. Scripts tune TVs from the lot's first ticks on,
// so the simulation and the materials learn the stations before those run.
void Game::startTvStations() {
    if (!broadcasts_) {
        try {
            broadcasts_ = std::make_shared<const ts2::content::Broadcasts>(
                ts2::content::Broadcasts::load(ts2::content::findProducts(install_), options_.user));
        } catch (const std::exception& e) {
            ts2::diag::warning(std::string("TV stations could not be read: ") + e.what());
            broadcasts_ = std::make_shared<const ts2::content::Broadcasts>();
        }
    }
    tvStations_ = std::make_unique<TvStations>(broadcasts_);
    tvClockMs_ = lotLife_->speedClockMs();
    auto* stations = tvStations_.get();
    lotLife_->simulation().world().tvLinks().setLookup([stations](std::string_view name) { return stations->station(name); });
    lotLife_->objectMaterials().setStations([stations](const std::string& name) { return stations->station(name).has_value(); });
}

// Movies run on the speed clock, so they hold at pause and run faster at fast and ultra speed, and a
// console game lasts the same Sim time at every speed. The game plays them in real time.
void Game::updateTvStations() {
    if (tvStations_ && lotLife_) {
        const std::int64_t now = lotLife_->speedClockMs();
        const auto elapsedMs = static_cast<double>(std::max<std::int64_t>(0, now - tvClockMs_));
        tvClockMs_ = now;
        tvStations_->update(lotLife_->simulation().world(), elapsedMs, lotLife_->objectMaterials(), renderer_);
    }
}

} // namespace app
