// The Graphics options panel's settings put into effect, and the frame rate limit.
#include "game.hpp"

#include "diag/log.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace app {

// The keep-these-settings box counts down wherever it is open, and may put the settings back.
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
    const bool newUiSize = display.uiScale != uiPercent_;
    uiPercent_ = display.uiScale;
    takeWindowSize(newUiSize);
    frameLimit_ = display.frameLimit;
    renderer_.setWaitForScreen(frameLimit_ == app::GraphicsOptionsPanel::kWaitForScreen);
    renderer_.setSmoothEdges(display.smoothEdges);
    renderer_.setAmbientOcclusion(ts2::content::kAmbientOcclusionStrengths[static_cast<std::size_t>(display.ambientOcclusion)]);
    renderer_.setAmbientOcclusionHalfSize(display.occlusionHalfSize);
}

// Waits out what is left of the frame under a frame rate limit.
void Game::paceFrame() {
    if (frameLimit_ <= 0) {
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
    frameDueNs_ = std::max(frameDueNs_, now) + 1'000'000'000ull / static_cast<std::uint64_t>(frameLimit_);
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
