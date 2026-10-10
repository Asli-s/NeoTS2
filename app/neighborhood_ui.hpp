#pragma once

#include "content/ui_data.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace app {

// The game's panels over a neighborhood: the puck in the bottom left corner, the top bar with the
// neighborhood's name, and the bins the puck opens (families without a home, lots to place), placed
// as the game places them.
class NeighborhoodUi {
public:
    NeighborhoodUi(ts2::ui::Screen& screen, ts2::content::UiData& data, int width, int height);

    // Shows the panels for a neighborhood (its folder in Neighborhoods and its name); `user` is the
    // game's user folder (for the lot catalog).
    void open(const std::filesystem::path& folder, const std::string& name, const std::filesystem::path& user);
    void close();

    enum class Bin { None, Families, Lots };
    void showBin(Bin bin);

    // A button released; returns true when the player asked for the main menu.
    bool clicked(int window);

    int root() const { return panel_; }
    // The lots a household lives on: those that can be played.
    const std::vector<std::uint32_t>& occupiedLots() const { return occupied_; }

private:
    struct Strip {
        int container = -1;         // the strip's row (window 0x1000)
        std::vector<int> tiles;     // the tiles placed in it
    };
    void layoutStrip(Strip& strip, const std::vector<ts2::tga::Pixels>& pictures);

    ts2::ui::Screen& screen_;
    ts2::content::UiData& data_;
    int width_, height_;
    int panel_ = -1, families_ = -1, lots_ = -1;
    int puck_ = -1;
    Strip familyStrip_, lotStrip_;
    std::vector<ts2::tga::Pixels> familyPictures_, lotPictures_;
    Bin bin_ = Bin::None;
    std::vector<std::uint32_t> occupied_;
};

} // namespace app
