#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>

struct MenuOptions {
    int width = 1280, height = 800;
    std::filesystem::path shot;  // draw one frame to this file and stop
    std::filesystem::path user;  // the game's user folder (Neighborhoods, Downloads, ...)
    int preview = -1;            // open this neighborhood's preview first
    int play = -1;               // open this neighborhood straight away
    std::string bin;             // and open its "families" or "lots" bin
    int lot = 0;                 // or enter this lot of it
    int clickX = -1, clickY = -1;  // or the lot under this point of the screen
    int hoverX = -1, hoverY = -1;  // the mouse rests here (for a picture with a tooltip)
    std::optional<std::array<float, 3>> view;  // the picture's camera: yaw, pitch (radians), distance
};

// Shows the game's main menu until it is closed.
int runMenu(const std::filesystem::path& install, const std::filesystem::path& shaders, const MenuOptions& options);
