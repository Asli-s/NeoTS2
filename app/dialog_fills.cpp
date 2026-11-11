#include "dialog_fills.hpp"

#include <algorithm>

namespace app {

void paintFills(ts2::ui::Screen& screen, int root, std::uint32_t group, std::uint32_t instance) {
    const auto script = screen.data().script(group, instance);
    if (!script || script->empty()) {
        return;
    }
    const auto& windows = script->front().children;
    const auto& made = screen.children(root);
    for (std::size_t i = 0; i < windows.size() && i < made.size(); ++i) {
        const auto& w = windows[i];
        const auto color = w.list("fillcolor");
        if (w.text("iid") != "IGZWinBMP" || w.get("image") || w.text("transparentbkg") != "no" || color.size() != 3) {
            continue;
        }
        // One pixel, which the window stretches over its area, whatever size a script gives it.
        ts2::tga::Pixels fill;
        fill.width = fill.height = 1;
        fill.rgba = {static_cast<std::uint8_t>(std::clamp<std::int64_t>(color[0], 0, 255)),
                     static_cast<std::uint8_t>(std::clamp<std::int64_t>(color[1], 0, 255)),
                     static_cast<std::uint8_t>(std::clamp<std::int64_t>(color[2], 0, 255)), 255};
        screen.setImage(made[i], std::move(fill));
    }
}

} // namespace app
