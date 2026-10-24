#pragma once

struct ImFont;

namespace ts2::launcher {

struct LauncherFonts {
    ImFont* text = nullptr;
    ImFont* heading = nullptr;  // section titles and the Play button
};

// Loads the built-in fonts and sets the launcher's look, sized for the display's scale
// (1.0 at 100% in Windows' display settings).
LauncherFonts applyStyle(float displayScale);

} // namespace ts2::launcher
