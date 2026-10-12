#include "mood_meter.hpp"

#include "content/ui_meters.hpp"

namespace app {

namespace {

constexpr std::uint32_t kImages = 0x499DB772;
// The meter's gradient (each column a whole meter), and per meter: its window, its fill's picture
// and its two platinum glows.
constexpr std::uint32_t kGradient = 0x000B0160;
constexpr std::uint32_t kFill = 3;
struct MeterWindows {
    std::uint32_t meter, picture, glow1, glow2;
};
constexpr MeterWindows kMeters[] = {
    {0x49064803, 0x000B0725, 0x49064805, 0x49064807},  // wants closed
    {0x49064804, 0x000B0715, 0x49064806, 0x49064808},  // wants open
};

} // namespace

void MoodMeter::attach(ts2::ui::Screen& screen, int panel) {
    screen_ = &screen;
    gradient_ = screen.data().image(kImages, kGradient).value_or(ts2::tga::Pixels{});
    for (std::size_t i = 0; i < meters_.size(); ++i) {
        const MeterWindows& w = kMeters[i];
        Meter& m = meters_[i];
        m.fill = screen.find(screen.find(panel, w.meter), kFill);
        m.picture = screen.data().image(kImages, w.picture).value_or(ts2::tga::Pixels{});
        m.glows = {screen.find(panel, w.glow1), screen.find(panel, w.glow2)};
    }
}

void MoodMeter::show(int mood, int aspirationScore) {
    if (screen_ == nullptr) {
        return;
    }
    const int value = ts2::content::moodMeterValue(mood, aspirationScore);
    const int column = ts2::content::meterRow(value, -100, 100, static_cast<int>(gradient_.width));
    const int platinum = ts2::content::isPlatinum(aspirationScore) ? 1 : 0;
    for (Meter& m : meters_) {
        if (column != m.column && gradient_.width > 0) {
            screen_->setImage(m.fill, ts2::content::paintColumns(m.picture, gradient_, column));
            m.column = column;
        }
        if (platinum != platinum_) {
            for (const int glow : m.glows) {
                screen_->setVisible(glow, platinum == 1);
            }
        }
    }
    platinum_ = platinum;
}

} // namespace app
