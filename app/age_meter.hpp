#pragma once

#include "content/ui_meters.hpp"
#include "formats/tga.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace app {

// The live panel's age display under the portrait: the picture of the selected Sim's life stage
// with a bar that fills as the stage goes by, and a tooltip with the days left.
class AgeMeter {
public:
    // `panel` is the loaded live panel script's root window.
    void attach(ts2::ui::Screen& screen, int panel);
    // The same for a panel with other stage pictures, `stageWindows` their ids in the ages container.
    void attach(ts2::ui::Screen& screen, int panel, std::span<const std::uint32_t> stageWindows);

    // Shows a life stage (person data 0x3A) with `lived` of its `length` days gone, and its tooltip.
    void show(std::int16_t lifeStage, int lived, int length, const ts2::content::AgeTip& tip);
    // The same with the stage picture given by its window id (0: none).
    void showStage(std::uint32_t stageWindow, int lived, int length, const ts2::content::AgeTip& tip);

private:
    struct Stage {
        int window = -1;
        int fill = -1;
    };

    ts2::ui::Screen* screen_ = nullptr;
    std::vector<std::pair<std::uint32_t, Stage>> stages_;
    ts2::tga::Pixels fill_;
    int tooltip_ = -1;
    ts2::content::AgeTip tip_;
    std::optional<std::uint32_t> stage_;  // the stage window shown, none until set
    std::uint32_t filled_ = UINT32_MAX;
};

} // namespace app
