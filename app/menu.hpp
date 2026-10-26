#pragma once

#include "render/make_renderer.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct MenuOptions {
    int width = 1280, height = 800;
    std::filesystem::path shot;  // draw one frame to this file and stop
    std::filesystem::path user;  // the game's user folder (Neighborhoods, Downloads, ...)
    bool addNeighborhoods = false;  // add the install's neighborhoods to a user folder with none
    int preview = -1;            // open this neighborhood's preview first
    int play = -1;               // open this neighborhood straight away
    std::string bin;             // and open its "families" or "lots" bin
    int lot = 0;                 // or enter this lot of it
    int clickX = -1, clickY = -1;  // or the lot under this point of the screen
    int cardX = -1, cardY = -1;    // or open the card of the lot under this point
    int hoverX = -1, hoverY = -1;  // the mouse rests here (for a picture with a tooltip)
    // Points of the lot's panels clicked before its picture: x, y and the kind (0 left click, 1 right
    // click, 2 the mouse resting there).
    std::vector<std::array<int, 3>> uiClicks;
    std::optional<std::array<float, 3>> view;  // the picture's camera: yaw, pitch (radians), distance
    // A point of the lot clicked before its picture (x, y) and the menu choice made (-1: left open).
    std::optional<std::array<int, 3>> groundClick;
    double runSeconds = 0;       // the lot plays on this long after the clicks before its picture
    bool unpaused = false;       // the lot starts with time running (the game opens lots paused)
    bool smoothFollow = false;   // the camera glides after a followed Sim (see Orbit::smoothFollow)
    bool bumpMapping = true;     // the game's bump mapping (High Material Detail)
    bool wallBumps = true;       // and of floors and walls (High Lighting Quality)
    ts2::render::Backend backend = ts2::render::defaultBackend();  // what draws (--renderer)
    bool timings = false;        // write the timing report (diag/timing.hpp) to the log every 10 seconds
    std::optional<std::uint32_t> seed;  // fixes a neighborhood's ground variants, so its pictures repeat
    // Asked once a frame: a warning about the install's files found by a check running beside the
    // game, given once, or nothing yet.
    std::function<std::string()> takeInstallWarning;
};

// Shows the game's main menu until it is closed.
int runMenu(const std::filesystem::path& install, const std::filesystem::path& shaders, const MenuOptions& options);
