#pragma once

#include "content/graphics_options.hpp"
#include "content/ui_data.hpp"
#include "dialog_box.hpp"
#include "options_list.hpp"
#include "options_menu.hpp"
#include "ui/screen.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace app {

// The options menu's Graphics/Performance Options panel (script 0x49060F03), in a lot's menu or the
// neighborhood's. A click only moves a radio button. Apply Changes puts the choices to work and asks
// whether to keep them, and they go back unless the player says Yes before the countdown ends. Only
// kept choices are saved to Config/userProps.xml.
class GraphicsOptionsPanel {
public:
    GraphicsOptionsPanel(ts2::ui::Screen& screen, OptionsMenu& menu) : screen_(screen), menu_(menu) {}

    // Reads the saved choices, the install's Graphics Rules and the keep-these-settings box's texts.
    void readSettings(ts2::content::UiData& data, const std::filesystem::path& install, const std::filesystem::path& userFolder);
    // The other panel's settings, read once and kept the same after a choice made there.
    void takeSettings(const GraphicsOptionsPanel& other);
    // Loads the panel hidden, inside the options menu. In a neighborhood Shadows is greyed.
    enum class Place { Lot, Neighborhood };
    void load(Place place);
    void remove();
    // Opens or closes the panel in its menu. Radio buttons moved but not applied go back on opening.
    void show(bool show);
    bool shown() const { return menu_.isOpen(panel_); }

    // Handles a click on the menu's Graphics button, the panel or the keep-these-settings box. True
    // when the window is one of those.
    bool clicked(int window);
    // Counts the box down, called each frame with the time in milliseconds.
    void tick(std::uint64_t ms);
    // True once after the settings in force have changed.
    bool takeChanged();
    // The setting number in force for a Graphics Rules option.
    int setting(const ts2::content::GraphicsGroup& group) const;
    // What the panel's display choices ask for, in force from Apply Changes on.
    struct Display {
        int width = 0, height = 0;
        bool fullscreen = false;
        // The frames a second aimed at: kWaitForScreen, kHalfScreen or kThirdScreen (every refresh, every
        // other or every third), kNoLimit or a number.
        int frameLimit = kWaitForScreen;
        // Samples each pixel takes to smooth edges: 1 for off, or 2, 4 or 8.
        int smoothEdges = 1;
        // How dark Ambient Occlusion is, an index into content::kAmbientOcclusionStrengths.
        int ambientOcclusion = 0;
        // Ambient Occlusion worked out at half the view's size, which costs less and is a little softer.
        bool occlusionHalfSize = true;
        // The UI drawn this much larger, in percent: one of content::kUiScales. Not in the original
        // game, so 100 unless the player picks another.
        int uiScale = 100;
        // Not in the original game: ponds and the sea drawn like pools, with a depth colour and
        // caustics, on lots and in the neighborhood (Water).
        bool realisticWater = false;
        // Not in the original game: the game's skies recoloured (see ts2::scene::recolourRealisticSky).
        bool realisticSky = false;
        // Not in the original game: the neighborhood ground's paints drawn finer (Terrain Detail), an
        // index into the panel's steps, 0 for the game's own.
        int terrainDetail = 0;
        bool operator==(const Display&) const = default;
    };
    static constexpr int kWaitForScreen = 0, kNoLimit = -1, kHalfScreen = -2, kThirdScreen = -3;
    const Display& display() const { return display_; }
    // The window's real size, which may differ from the one asked for.
    void setWindowSize(int width, int height);
    // The UI Size in force, which --ui-scale may have given instead of the saved one.
    void setUiScale(int percent);
    // True once after the display choices in force have changed.
    bool takeDisplayChanged();
    // Reads the saved display choices without a panel, for the game's start.
    static Display savedDisplay(const std::filesystem::path& userFolder);
    // The tiles one picture of the neighborhood ground's paints covers at a Terrain Detail step, 0
    // for the ground's own (see IRenderer::setTerrainTiles).
    static float terrainTiles(int detail);
    // The refresh rate of the window's screen in Hz, 0 when not known, for the Frame Rate list.
    void setScreenRefreshRate(double hz);
    // The screen has a new size: the keep-these-settings box stays in its middle.
    void centreBox();
    // Fills the Screen Size list with the sizes the screen offers.
    void listScreenSizes();
    // The most samples this card can smooth edges with. Smooth Edges offers no more than that.
    void setMostSmoothEdges(int samples);
    // The mouse on the panel's scroll bars, in UI pixels (see OptionsList).
    bool mouseDown(int x, int y);
    void mouseMove(int x, int y);
    void mouseUp(int x, int y);
    bool holdsMouse() const { return screenSizes_.holdsMouse() || frameRates_.holdsMouse(); }
    bool wheel(int x, int y, int delta);
    // A true or false property as the option's setting in force gives it in Graphics Rules, true
    // when the rules don't say.
    bool propOn(std::string_view option, std::string_view key) const;
    // A number the chosen setting of an option gives, `otherwise` when it gives none or a broken one.
    int propNumber(std::string_view option, std::string_view key, int otherwise) const;

private:
    void showChoices();
    void pick(int group, int button);
    void pickDefaults();
    void apply();
    void keep();
    void putBack();
    // The setting a group's checked radio button picks, -1 when none is checked.
    int checkedSetting(const ts2::content::GraphicsGroup& group) const;
    void checkFullscreen(bool on);
    void showDisplay();
    Display pickedDisplay() const;
    void listFrameRates();
    // Picks the Frame Rate list's row for `limit`, or none when it has none.
    void pickFrameRate(int limit);
    // Makes the panel taller than its script does, with room for Ambient Occlusion and a longer Screen Size list.
    // Moves the script's `lowest` groups down below `bottom` (in the panel's pixels), stretching the
    // panel's picture to match.
    void makeTaller(const std::vector<int>& lowest, int bottom);
    // Replaces the game's slider with radio buttons for its levels, and adds Ambient Occlusion's under them.
    void addSmoothEdgeRows();
    void addUiSizeRows();
    // NeoTS2's own groups under the panel's: Ambient Occlusion, AO Quality, Sky, Water and Terrain Detail. Returns the
    // lowest one's bottom in the panel's pixels.
    int addOwnGroups();
    // A titled group of radio buttons at (x, y) in the panel, one row each.
    int addChoiceGroup(std::uint32_t id, int x, int y, const std::string& title, const std::vector<std::pair<std::uint32_t, std::string>>& rows);
    bool ownGroup(int window) const;
    int addRadioRow(int group, std::uint32_t id, int top, const std::string& text, int left = 0);
    void addGroupTitle(int group, int top, const std::string& text);

    ts2::ui::Screen& screen_;
    OptionsMenu& menu_;
    int panel_ = -1;
    std::filesystem::path userFolder_;
    std::string graphicsRules_;  // the install's Graphics Rules.sgr
    std::map<std::string, std::string, std::less<>> values_;  // the choices in force, by saved key
    std::map<std::string, std::string, std::less<>> before_;  // what they were before Apply Changes
    bool changed_ = false;
    Display display_, displayBefore_;
    bool displayChanged_ = false;
    OptionsList screenSizes_, frameRates_;
    // The Screen Size picked or saved, which the window may not have when the screen's free area is smaller.
    std::array<int, 2> askedSize_{}, askedBefore_{};
    std::vector<int> rateLimits_;  // the Frame Rate list's rows, as Display::frameLimit
    double refreshRate_ = 0;
    int mostSmoothEdges_ = 1;
    std::vector<std::array<int, 2>> sizes_;  // the Screen Size list's rows

    DialogBox box_;
    std::string boxTitle_, boxText_, yes_, no_;
    int countdown_ = 0;
    std::uint64_t nextCount_ = 0, now_ = 0;
};

} // namespace app
