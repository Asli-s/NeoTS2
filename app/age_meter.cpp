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

void AgeMeter::attach(ts2::ui::Screen& screen, int panel) {
    screen_ = &screen;
    const int ages = screen.find(panel, kAges);
    tooltip_ = screen.find(ages, kTooltip);
    for (std::size_t i = 0; i < stages_.size(); ++i) {
        stages_[i].window = screen.find(ages, kStageWindows[i]);
        stages_[i].fill = screen.find(screen.find(stages_[i].window, kProgressBar), kFill);
        // The glows only play when the Sim ages, at 6 pm.
        if (stages_[i].window >= 0) {
            for (const int part : screen.children(stages_[i].window)) {
                if (screen.id(part) == kGlow || screen.id(part) == kBarGlow) {
                    screen.setVisible(part, false);
                }
            }
        }
    }
    fill_ = screen.data().image(kImages, kFillPicture).value_or(ts2::tga::Pixels{});
}

void AgeMeter::show(std::int16_t lifeStage, int lived, int length, const ts2::content::AgeTip& tip) {
    if (screen_ == nullptr) {
        return;
    }
    if (tip.entry != tip_.entry || tip.days != tip_.days) {
        tip_ = tip;
        const std::string text = tip.entry != 0 ? screen_->data().text(kLiveText, kAgeTexts << 16 | tip.entry) : std::string();
        screen_->setTip(tooltip_, ts2::content::formatted(text, {std::to_string(tip.days)}));
    }
    const int stage = ts2::content::ageStage(lifeStage);
    const std::uint32_t filled = ts2::content::barFill(lived, length, fill_.width);
    if (stage == stage_ && filled == filled_) {
        return;
    }
    for (int i = 0; i < static_cast<int>(stages_.size()); ++i) {
        screen_->setVisible(stages_[static_cast<std::size_t>(i)].window, i == stage);
    }
    if (stage >= 0) {
        // The bar's fill is cut at the filled width.
        const int fill = stages_[static_cast<std::size_t>(stage)].fill;
        screen_->setVisible(fill, filled > 0);
        if (filled > 0) {
            screen_->setImage(fill, ts2::content::placed(fill_, filled, fill_.height, 0, 0));
            screen_->resize(fill, static_cast<int>(filled), static_cast<int>(fill_.height));
        }
    }
    stage_ = stage;
    filled_ = filled;
}

} // namespace app
