// The Graphics options panel's settings put into effect, and the frame rate limit.
#include "game.hpp"

#include "diag/log.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace app {

// The keep-these-settings box counts down wherever it is open, and may put the settings back. A
// held press on a list's scroll bar repeats.
void Game::tickGraphicsOptions(std::uint64_t ms) {
    auto& lot = lotUi_.graphicsOptions();
    auto& hood = hoodUi_.graphicsOptions();
    lot.tick(ms);
    hood.tick(ms);
    if (lot.takeChanged()) {
        hood.takeSettings(lot);
        applyGraphicsOptions();
        applyDisplayOptions();
    }
    if (hood.takeChanged()) {
        lot.takeSettings(hood);
        applyGraphicsOptions();
        applyDisplayOptions();
    }
}

// Puts the display choices in force to work, once after they change.
void Game::applyDisplayOptions() {
    auto& lot = lotUi_.graphicsOptions();
    auto& hood = hoodUi_.graphicsOptions();
    const bool lotChanged = lot.takeDisplayChanged(), hoodChanged = hood.takeDisplayChanged();
    if (!lotChanged && !hoodChanged) {
        return;
    }
    const auto display = (lotChanged ? lot : hood).display();
    ts2::platform::setFirstWindowFullscreen(display.fullscreen);
    if (!display.fullscreen) {
        ts2::platform::resizeFirstWindow(display.width, display.height);
    }
    // A new UI Size lays the UI out again even when the window keeps its size.
    // A new Water shows on the open lot's ponds from the next lot built: building the open lot again
    // from its file would undo what happened on it since it opened. The sea and the neighborhood are
    // put to work at once.
    const bool ponds = display.realisticWater || options_.poolStylePonds;
    const bool sea = display.realisticWater || options_.poolStyleSea;
    if (ponds != realisticPonds_ || sea != realisticSea_) {
        realisticPonds_ = ponds;
        realisticSea_ = sea;
        applySeaWater();
    }
    // Skies read from now on, such as the next time of day's, take the Sky chosen.
    realisticSky_ = display.realisticSky;
    assets_.get()->setRealisticSky(realisticSky_);
    const bool newUiSize = display.uiScale != uiPercent_;
    uiPercent_ = display.uiScale;
    takeWindowSize(newUiSize);
    frameLimit_ = display.frameLimit;
    applyFrameLimit();
    renderer_.setSmoothEdges(display.smoothEdges);
    renderer_.setAmbientOcclusion(ts2::content::kAmbientOcclusionStrengths[static_cast<std::size_t>(display.ambientOcclusion)]);
    renderer_.setAmbientOcclusionHalfSize(display.occlusionHalfSize);
    renderer_.setTerrainTiles(app::GraphicsOptionsPanel::terrainTiles(display.terrainDetail));
}

// Picks how frames are paced under the Frame Rate chosen. Half and Third (of the screen's rate), and a rate that
// divides the screen's refresh rate (60 on a 120 Hz screen), wait for the screen and show each frame for
// the same number of refreshes, so frames stay evenly apart. A frame timed by the clock alone reaches the
// screen at a different point of its refresh each time, and stays up for one refresh or three. Any
// other rate is kept by the clock.
void Game::applyFrameLimit() {
    using Panel = app::GraphicsOptionsPanel;
    refreshRate_ = renderer_.screenRefreshRate();
    for (auto* panel : {&lotUi_.graphicsOptions(), &hoodUi_.graphicsOptions()}) {
        panel->setScreenRefreshRate(refreshRate_);
    }
    refreshesPerFrame_ = 0;
    clockRate_ = frameLimit_ > 0 ? frameLimit_ : 0;
    const int screenShare = frameLimit_ == Panel::kHalfScreen ? 2 : frameLimit_ == Panel::kThirdScreen ? 3 : 0;
    if (screenShare > 0) {
        refreshesPerFrame_ = refreshRate_ > 0 ? screenShare : 0;
        // A screen whose rate isn't known is taken as 60 Hz.
        clockRate_ = 60 / screenShare;
    } else if (frameLimit_ > 0 && refreshRate_ > 0) {
        // Screens run a little off their round rates, as 119.88 Hz for 120.
        const double ratio = refreshRate_ / frameLimit_;
        const long whole = std::lround(ratio);
        if (whole >= 1 && std::abs(ratio - static_cast<double>(whole)) < 0.02 * static_cast<double>(whole)) {
            refreshesPerFrame_ = static_cast<int>(whole);
        }
    }
    renderer_.setWaitForScreen(frameLimit_ == Panel::kWaitForScreen || refreshesPerFrame_ > 0);
    renderer_.setKeepFrames(refreshesPerFrame_ > 1);
}

// Waits out what is left of the frame under a frame rate limit.
void Game::paceFrame() {
    if (frameLimit_ == app::GraphicsOptionsPanel::kWaitForScreen || frameLimit_ == app::GraphicsOptionsPanel::kNoLimit) {
        return;
    }
    // The window may have moved to a screen with another refresh rate.
    if (renderer_.screenRefreshRate() != refreshRate_) {
        applyFrameLimit();
    }
    if (refreshesPerFrame_ > 0) {
        bool shown = true;
        for (int i = 1; i < refreshesPerFrame_ && shown; ++i) {
            shown = renderer_.showAgain();
        }
        if (shown) {
            frameDueNs_ = 0;
            return;
        }
    }
    if (clockRate_ <= 0) {
        return;
    }
    const std::uint64_t now = ts2::platform::ticksNs();
    if (frameDueNs_ > now) {
        // The last millisecond is waited in a spin, as a sleep may overshoot it.
        const std::uint64_t left = frameDueNs_ - now;
        if (left > 2'000'000) {
            ts2::platform::delayMs(static_cast<std::uint32_t>(left / 1'000'000 - 1));
        }
        while (ts2::platform::ticksNs() < frameDueNs_) {
        }
    }
    frameDueNs_ = std::max(frameDueNs_, now) + 1'000'000'000ull / static_cast<std::uint64_t>(clockRate_);
}

// Graphic Effects: which of an effect's versions play, and how many particles they try to stay under.
void Game::applyEffectOptions() {
    constexpr int kHighestLevel = 3, kMostParticles = 10000;
    const auto& graphics = lotUi_.graphicsOptions();
    for (auto* player : {lotEffectPlayer_.get(), hoodEffectPlayer_.get()}) {
        if (player) {
            player->setLevel(static_cast<std::uint16_t>(std::max(graphics.propNumber("EffectsQuality", "effectPriorityLevel", kHighestLevel), 0)));
            player->setParticleTarget(static_cast<std::uint32_t>(std::max(graphics.propNumber("EffectsQuality", "maxParticlesTarget", kMostParticles), 0)));
        }
    }
}

// The Graphics options in force, unless the command line turned a feature off.
void Game::applyGraphicsOptions() {
    const auto& graphics = lotUi_.graphicsOptions();
    applyEffectOptions();
    renderer_.setBumpMapping(options_.bumpMapping && graphics.propOn("MaterialDetail", "bumpMapping"));
    renderer_.setFloorAndWallNormalMapping(options_.wallBumps && graphics.propOn("LightingQuality", "floorAndWallNormalMapping"));
    renderer_.setSpecularHighlights(graphics.propOn("LightingQuality", "specHighlights"));
    renderer_.setShadowOptions({graphics.propOn("Shadows", "simShadows"), graphics.propOn("Shadows", "objectShadows"), graphics.propOn("Shadows", "guob")});
    renderer_.setReflections(graphics.propOn("Reflection", "reflectionWithExtraViewer"));
    const app::LotLightOptions light{graphics.propOn("Shadows", "heightMapShadows"), graphics.propOn("LightingQuality", "portalLighting")};
    if (light.sunShadow != lotLight_.sunShadow || light.portals != lotLight_.portals) {
        lotLight_ = light;
        app::setLotLightOptions(light);
        if (lotDaylight_) {
            lotDaylight_->lightAgain();
        }
    }
}

} // namespace app
