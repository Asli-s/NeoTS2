#pragma once

#include "ui/screen.hpp"

#include <cstdint>

namespace app {

// Gives the picture windows of a loaded UI script that have a fill colour but no picture
// (transparentbkg no) a picture of that colour, as the game paints them. The screen doesn't paint
// such fills itself yet (UI-56). `root` is the window the script was loaded as.
void paintFills(ts2::ui::Screen& screen, int root, std::uint32_t group, std::uint32_t instance);

} // namespace app
