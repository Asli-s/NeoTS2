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
    bool fitScreen = false;      // the window fills the screen's free area instead (see WindowOptions::fitScreen)
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
    bool saveNeighborhood = false;  // the neighborhood's decorations saved after the clicks (a test)
    double recordSeconds = 0;  // a Video Capture this long before the picture (a test)
    std::string type;  // typed after the clicks into the story editor box they focused, then saved (a test)
    std::optional<std::array<float, 3>> view;  // the picture's camera: yaw, pitch (radians), distance
    // A point of the lot clicked before its picture (x, y) and the menu choice made (-1: left open).
    std::optional<std::array<int, 3>> groundClick;
    // An object's menu choice made for the selected Sim once the lot comes to life: object id and the
    // choice's place in its menu.
    std::optional<std::array<int, 2>> choose;
    double runSeconds = 0;       // the lot plays on this long after the clicks before its picture
    bool unpaused = false;       // the lot starts with time running (the game opens lots paused)
    bool noGameTips = false;     // the Game Tips question is answered No (see World::setDeclinesGameTips)
    bool smoothFollow = false;   // the camera glides after a followed Sim (see Orbit::smoothFollow)
    // Realistic ponds and sea whatever the Graphics options keep (see
    // SceneBuilder::setPoolStylePonds and setPoolStyleSea).
    bool poolStylePonds = false;
    bool poolStyleSea = false;
    bool smoothLotSkirts = false; // lots joined to the ground around them without seams (see SceneBuilder::setSmoothLotSkirts)
    // The UI drawn this much larger, in percent: one of content::kUiScales. The game has 100 only.
    // A window too small for it gets less, so the whole UI still fits.
    int uiScale = 100;
    bool uiScaleGiven = false;   // --ui-scale was given, so the UI doesn't take the saved size
    bool bumpMapping = true;     // the game's bump mapping (High Material Detail)
    bool wallBumps = true;       // and of floors and walls (High Lighting Quality)
    bool sizeGiven = false;      // --size was given, so the window doesn't take the saved one
    ts2::render::Backend backend = ts2::render::defaultBackend();  // what draws (--renderer)
    bool timings = false;        // write the timing report (diag/timing.hpp) to the log every 10 seconds
    std::optional<std::uint32_t> seed;  // fixes a neighborhood's ground variants, so its pictures repeat
    // Asked once a frame: a warning about the install's files found by a check running beside the
    // game, given once, or nothing yet.
    std::function<std::string()> takeInstallWarning;
};

// Shows the game's main menu until it is closed.
int runMenu(const std::filesystem::path& install, const std::filesystem::path& shaders, const MenuOptions& options);
