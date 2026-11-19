#include "age_meter.hpp"

#include "content/ui_meters.hpp"
#include "content/ui_text.hpp"

#include <string>

namespace app {

namespace {

constexpr std::uint32_t kImages = 0x499DB772;
constexpr std::uint32_t kAges = 0x2E9AF844;
// A button over the pictures that only carries the tooltip.
constexpr std::uint32_t kTooltip = 0xAD8A9F10;
// The tooltip texts: Live text table 0x83.
constexpr std::uint32_t kLiveText = 0x7F96C284, kAgeTexts = 0x83;
// Toddler, child, teen, adult and elder.
constexpr std::uint32_t kStageWindows[] = {0xAD8A9F0B, 0xAD8A9F0C, 0xAD8A9F0D, 0xAD8A9F0E, 0xAD8A9F0F};
constexpr std::uint32_t kProgressBar = 1, kFill = 3;
// The stage figure's glow and the bar's, beside the bar (the bar's fill is also numbered 3).
constexpr std::uint32_t kGlow = 2, kBarGlow = 3;
constexpr std::uint32_t kFillPicture = 0x000B1180;

} // namespace

void AgeMeter::attach(ts2::ui::Screen& screen, int panel) { attach(screen, panel, kStageWindows); }

void AgeMeter::attach(ts2::ui::Screen& screen, int panel, std::span<const std::uint32_t> stageWindows) {
    screen_ = &screen;
    const int ages = screen.find(panel, kAges);
    tooltip_ = screen.find(ages, kTooltip);
    stages_.clear();
    for (const std::uint32_t id : stageWindows) {
        Stage stage;
        stage.window = screen.find(ages, id);
        stage.fill = screen.find(screen.find(stage.window, kProgressBar), kFill);
        stages_.emplace_back(id, stage);
        // The glows only play when the Sim ages, at 6 pm.
        if (stage.window >= 0) {
            for (const int part : screen.children(stage.window)) {
                if (screen.id(part) == kGlow || screen.id(part) == kBarGlow) {
                    screen.setVisible(part, false);
                }
            }
        }
    }
    fill_ = screen.data().image(kImages, kFillPicture).value_or(ts2::tga::Pixels{});
}

void AgeMeter::show(std::int16_t lifeStage, int lived, int length, const ts2::content::AgeTip& tip) {
    const int stage = ts2::content::ageStage(lifeStage);
    showStage(stage >= 0 ? kStageWindows[stage] : 0, lived, length, tip);
}

void AgeMeter::showStage(std::uint32_t stageWindow, int lived, int length, const ts2::content::AgeTip& tip) {
    if (screen_ == nullptr) {
        return;
    }
    if (tip.entry != tip_.entry || tip.days != tip_.days) {
        tip_ = tip;
        const std::string text = tip.entry != 0 ? screen_->data().text(kLiveText, kAgeTexts << 16 | tip.entry) : std::string();
        screen_->setTip(tooltip_, ts2::content::formatted(text, {std::to_string(tip.days)}));
    }
    const std::uint32_t filled = ts2::content::barFill(lived, length, fill_.width);
    if (stage_ == stageWindow && filled == filled_) {
        return;
    }
    int fill = -1;
    for (const auto& [id, stage] : stages_) {
        screen_->setVisible(stage.window, id == stageWindow);
        if (id == stageWindow) {
            fill = stage.fill;
        }
    }
    if (fill >= 0) {
        // The bar's fill is cut at the filled width.
        screen_->setVisible(fill, filled > 0);
        if (filled > 0) {
            screen_->setImage(fill, ts2::content::placed(fill_, filled, fill_.height, 0, 0));
            screen_->resize(fill, static_cast<int>(filled), static_cast<int>(fill_.height));
        }
    }
    stage_ = stageWindow;
    filled_ = filled;
}

} // namespace app
