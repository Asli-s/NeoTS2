#include "lot_ui.hpp"

#include <string>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11;
constexpr std::uint32_t kPuck = 0x4905F85B, kLivePanel = 0x49060005, kOptionsMenu = 0x49060001;
// The puck's windows.
constexpr std::uint32_t kLiveMode = 0x37D97B25, kOptions = 0x67D97B2F, kFunds = 0x00003000;
// The live panel's windows.
constexpr std::uint32_t kClock = 0x37D82A93, kPause = 0x27D7258F, kNormal = 0x57E01CC1, kHigh = 0x37E01CC3,
                        kUltra = 0x47E01CC5;
constexpr std::uint32_t kNoPetsBackground = 0xA94E97E4, kPetsBackground = 0xA94E97E3;
constexpr std::uint32_t kPortraitHolder = 0x4BDEFFA4, kVacationDays = 0x1320BD2F;
// The options menu's windows.
constexpr std::uint32_t kNeighborhood = 0x0000A210, kExitTutorial = 0x0000A211;

// Funds as the puck shows them: the Simoleon sign and thousands separators.
std::string fundsText(std::int32_t funds) {
    std::string digits = std::to_string(funds < 0 ? -funds : funds);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<std::size_t>(i), ",");
    }
    return std::string(funds < 0 ? "-" : "") + "\xC2\xA7" + digits;
}

} // namespace

LotUi::LotUi(ts2::ui::Screen& screen, int width, int height) : screen_(screen), width_(width), height_(height) {}

void LotUi::open() {
    if (puck_ < 0) {
        puck_ = screen_.load(kScripts, kPuck, 0, 0);
        live_ = screen_.load(kScripts, kLivePanel, 0, 0);
        options_ = screen_.load(kScripts, kOptionsMenu, 0, 0);
        if (puck_ < 0 || live_ < 0 || options_ < 0) {
            return;
        }
        // The puck sits in the bottom left corner; the panels hang off it.
        screen_.move(puck_, 0, height_ - screen_.height(puck_));
        anchor(live_);
        anchor(options_);
        screen_.raise(puck_);
        // The clock and speed buttons, and the frame the live panel lays out; the rest (portrait,
        // influence and aspiration meters) isn't built yet.
        const int clock = screen_.find(live_, kClock), pause = screen_.find(live_, kPause);
        const int speeds = screen_.parent(screen_.find(live_, kNormal));
        for (const int part : screen_.children(live_)) {
            screen_.setVisible(part, part == clock || part == pause || part == speeds);
        }
        panel_.attach(screen_, live_);
        screen_.setVisible(screen_.find(live_, kPortraitHolder), false);
        screen_.setVisible(screen_.find(live_, kVacationDays), false);
        // A household without pets shows the plain background.
        screen_.setVisible(screen_.find(live_, kPetsBackground), false);
        screen_.setVisible(screen_.find(live_, kNoPetsBackground), true);
        screen_.setChecked(screen_.find(puck_, kLiveMode), true);
        // Exit Tutorial shares the Neighborhood button's place; it only shows in the tutorial.
        screen_.setVisible(screen_.find(options_, kExitTutorial), false);
    }
    for (const int panel : {puck_, live_}) {
        if (panel >= 0) {
            screen_.setVisible(panel, true);
        }
    }
    showOptions(false);
}

void LotUi::close() {
    for (const int panel : {puck_, live_, options_}) {
        if (panel >= 0) {
            screen_.setVisible(panel, false);
        }
    }
}

int LotUi::left(int window) const {
    int x = 0;
    for (int w = window; w >= 0; w = screen_.parent(w)) {
        x += screen_.x(w);
    }
    return x;
}

int LotUi::top(int window) const {
    int y = 0;
    for (int w = window; w >= 0; w = screen_.parent(w)) {
        y += screen_.y(w);
    }
    return y;
}

void LotUi::anchor(int panel) {
    // The anchor is a direct or nested child; its caption names the puck window it belongs on.
    auto find = [&](auto&& self, int window) -> int {
        for (const int c : screen_.children(window)) {
            const std::string& caption = screen_.text(c);
            if (caption.size() > 3 && caption.ends_with(":bl")) {
                return c;
            }
            if (const int found = self(self, c); found >= 0) {
                return found;
            }
        }
        return -1;
    };
    const int mark = find(find, panel);
    if (mark < 0) {
        return;
    }
    const std::string& caption = screen_.text(mark);
    const auto target = screen_.find(puck_, static_cast<std::uint32_t>(std::stoul(caption.substr(0, caption.size() - 3), nullptr, 16)));
    if (target < 0) {
        return;
    }
    screen_.move(panel, left(target) - (left(mark) - screen_.x(panel)), top(target) - (top(mark) - screen_.y(panel)));
}

void LotUi::showOptions(bool show) {
    if (options_ < 0) {
        return;
    }
    screen_.setVisible(options_, show);
    screen_.setVisible(live_, !show);
    screen_.setChecked(screen_.find(puck_, kOptions), show);
    screen_.setChecked(screen_.find(puck_, kLiveMode), !show);
}

bool LotUi::optionsShown() const { return options_ >= 0 && screen_.visible(options_); }

LotUi::Action LotUi::clicked(int window) {
    const std::uint32_t id = screen_.id(window);
    if (id == kOptions && screen_.parent(window) == puck_) {
        showOptions(!optionsShown());
        return optionsShown() ? Action::OptionsOpened : Action::OptionsClosed;
    }
    if (id == kLiveMode && screen_.parent(window) == puck_ && optionsShown()) {
        showOptions(false);
        return Action::OptionsClosed;
    }
    if (panel_.clicked(window)) {
        return Action::None;
    }
    if (id == kNeighborhood && optionsShown()) {
        return Action::Neighborhood;
    }
    switch (id) {
    case kPause: return Action::Pause;
    case kNormal: return Action::Normal;
    case kHigh: return Action::High;
    case kUltra: return Action::Ultra;
    default: return Action::None;
    }
}

void LotUi::show(const std::string& clock, ts2::sim::Speed speed, bool paused, std::int32_t funds) {
    if (live_ < 0) {
        return;
    }
    screen_.setText(screen_.find(live_, kClock), clock);
    screen_.setText(screen_.find(puck_, kFunds), fundsText(funds));
    using ts2::sim::Speed;
    screen_.setChecked(screen_.find(live_, kPause), paused);
    screen_.setChecked(screen_.find(live_, kNormal), !paused && speed == Speed::Normal);
    screen_.setChecked(screen_.find(live_, kHigh), !paused && speed == Speed::High);
    screen_.setChecked(screen_.find(live_, kUltra), !paused && speed == Speed::Ultra);
}

} // namespace app
