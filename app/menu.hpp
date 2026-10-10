#pragma once

#include <filesystem>

struct MenuOptions {
    int width = 1280, height = 800;
    std::filesystem::path shot;  // draw one frame to this file and stop
    std::filesystem::path user;  // the game's user folder (Neighborhoods, Downloads, ...)
    int preview = -1;            // open this neighborhood's preview first
    int hoverX = -1, hoverY = -1;  // the mouse rests here (for a picture with a tooltip)
};

// Shows the game's main menu until it is closed.
int runMenu(const std::filesystem::path& install, const std::filesystem::path& shaders, const MenuOptions& options);
