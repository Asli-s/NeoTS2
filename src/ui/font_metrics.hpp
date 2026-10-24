#pragma once

namespace ts2::ui {

// Where lines of a Type 1 font sit, as the game's own font engine measures them at a pixel size:
// the baseline's distance below the line's top, and the line's height.
struct LineMetrics {
    int ascent = 0;
    int height = 0;
};

// The game's measures from a Type 1 font's ascender and descender in font units (descender below
// zero) and its units per em. Without its own values the game takes the top of 'f' and the bottom
// of 'g' (see Fonts), and 1000 units per em. Sizes are clamped to 1..2048 pixels as the game does.
LineMetrics typeOneLine(int ascender, int descender, int unitsPerEm, int size);

} // namespace ts2::ui
