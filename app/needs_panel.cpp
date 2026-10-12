#include "needs_panel.hpp"

#include "content/ui_meters.hpp"
#include "sim/motives.hpp"

namespace app {

namespace {

namespace motive = ts2::sim::motive;

constexpr std::uint32_t kScripts = 0xA99D8A11, kImages = 0x499DB772;
// A needs bar: its script, its fill picture and the gradient it is coloured from.
constexpr std::uint32_t kBarScript = 0x4906500B;
constexpr std::uint32_t kFillPicture = 0x000B2050, kBarGradient = 0x000B2060;
// The bar script's windows: the rising and falling arrows, the bar, its fill and the name.
constexpr std::uint32_t kArrows[] = {1, 2, 3, 4, 5, 6};
constexpr std::uint32_t kBar = 7, kFill = 3, kName = 8;
// The needs script's layouts.
constexpr std::uint32_t kToddler = 0xA003, kChild = 0xA004, kTeenAdultElder = 0xA005;
constexpr std::uint32_t kOtherLayouts[] = {0xA006, 0xA007, 0xA008, 0xA009, 0xA00A};
// Life stages (person data 0x3A) with their own layout.
constexpr std::int16_t kToddlerStage = 2, kChildStage = 3;

// The need names: string table 0x82 of the Live text package (group 0x7F96C284).
constexpr std::uint32_t kLiveText = 0x7F96C284, kNeedNames = 0x82;
std::uint32_t nameString(std::size_t m) {
    switch (m) {
    case motive::kHunger: return 0x02;
    case motive::kEnergy: return 0x04;
    case motive::kComfort: return 0x06;
    case motive::kFun: return 0x08;
    case motive::kHygiene: return 0x0A;
    case motive::kSocial: return 0x0C;
    case motive::kBladder: return 0x0E;
    case motive::kEnvironment: return 0x10;
    default: return 0;
    }
}

} // namespace

void NeedsPanel::attach(ts2::ui::Screen& screen, int content) {
    screen_ = &screen;
    content_ = content;
    fill_ = screen.data().image(kImages, kFillPicture).value_or(ts2::tga::Pixels{});
    gradient_ = screen.data().image(kImages, kBarGradient).value_or(ts2::tga::Pixels{});
    // Each layout's slots and the motive each shows.
    layouts_[0] = build(kToddler, {{0xA1, motive::kHunger}, {0xA7, motive::kHygiene}, {0xA3, motive::kBladder},
                                   {0xA4, motive::kEnergy}, {0xA5, motive::kFun}, {0xA6, motive::kSocial}});
    layouts_[1] = build(kChild, {{0xA1, motive::kHunger}, {0xA2, motive::kComfort}, {0xA7, motive::kHygiene}, {0xA3, motive::kBladder},
                                 {0xA4, motive::kEnergy}, {0xA5, motive::kFun}, {0xA6, motive::kSocial}});
    layouts_[2] = build(kTeenAdultElder, {{0xA1, motive::kHunger}, {0xA2, motive::kComfort}, {0xA7, motive::kHygiene},
                                          {0xA3, motive::kBladder}, {0xA4, motive::kEnergy}, {0xA5, motive::kFun},
                                          {0xA6, motive::kSocial}, {0xA8, motive::kEnvironment}});
    for (const std::uint32_t id : kOtherLayouts) {
        screen.setVisible(screen.find(content, id), false);
    }
}

NeedsPanel::Layout NeedsPanel::build(std::uint32_t group, const std::vector<std::pair<std::uint32_t, std::size_t>>& slots) {
    auto& s = *screen_;
    Layout layout;
    layout.group = s.find(content_, group);
    for (const auto& [slotId, m] : slots) {
        // The bar script has several top windows, so its windows are looked up in the slot.
        const int bar = s.find(layout.group, slotId);
        if (bar < 0 || s.load(kScripts, kBarScript, 0, 0, bar) < 0) {
            continue;
        }
        // How fast a need changes (the arrows) is not shown yet.
        for (const std::uint32_t arrow : kArrows) {
            s.setVisible(s.find(bar, arrow), false);
        }
        s.setText(s.find(bar, kName), s.data().text(kLiveText, kNeedNames << 16 | nameString(m)));
        layout.bars.push_back({m, s.find(s.find(bar, kBar), kFill), -1});
    }
    return layout;
}

void NeedsPanel::show(std::int16_t lifeStage, const std::array<int, 16>& motives) {
    if (screen_ == nullptr) {
        return;
    }
    const std::size_t shown = lifeStage == kToddlerStage ? 0 : lifeStage == kChildStage ? 1 : 2;
    for (std::size_t i = 0; i < layouts_.size(); ++i) {
        screen_->setVisible(layouts_[i].group, i == shown);
    }
    for (Bar& bar : layouts_[shown].bars) {
        const int row = ts2::content::meterRow(motives[bar.motive], -100, 100, static_cast<int>(gradient_.height));
        if (row != bar.row) {
            screen_->setImage(bar.fill, ts2::content::paintBar(fill_, gradient_, row));
            bar.row = row;
        }
    }
}

} // namespace app
