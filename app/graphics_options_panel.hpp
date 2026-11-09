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
        // The frames a second aimed at: kWaitForScreen, kNoLimit or a number.
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
        bool operator==(const Display&) const = default;
    };
    static constexpr int kWaitForScreen = 0, kNoLimit = -1;
    const Display& display() const { return display_; }
    // The window's real size, which may differ from the one asked for.
    void setWindowSize(int width, int height);
    // The UI Size in force, which --ui-scale may have given instead of the saved one.
    void setUiScale(int percent);
    // True once after the display choices in force have changed.
    bool takeDisplayChanged();
    // Reads the saved display choices without a panel, for the game's start.
    static Display savedDisplay(const std::filesystem::path& userFolder);
    // The screen has a new size: the keep-these-settings box stays in its middle.
    void centreBox();
    // Fills the Screen Size list with the sizes the screen offers.
    void listScreenSizes();
    // The most samples this card can smooth edges with. Smooth Edges offers no more than that.
    void setMostSmoothEdges(int samples);
    // The mouse on the panel's scroll bars, in UI pixels (see OptionsList).
    bool mouseDown(int x, int y);
    void mouseMove(int y);
    void mouseUp();
    bool holdsMouse() const { return screenSizes_.holdsMouse() || frameRates_.holdsMouse(); }
    bool wheel(int x, int y, int rows);
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
    // Makes the panel taller than its script does, with room for Ambient Occlusion and a longer Screen Size list.
    void makeTaller();
    // Replaces the game's slider with radio buttons for its levels, and adds Ambient Occlusion's under them.
    void addSmoothEdgeRows();
    void addUiSizeRows();
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
    int mostSmoothEdges_ = 1;
    std::vector<std::array<int, 2>> sizes_;  // the Screen Size list's rows

    DialogBox box_;
    std::string boxTitle_, boxText_, yes_, no_;
    int countdown_ = 0;
    std::uint64_t nextCount_ = 0, now_ = 0;
};

} // namespace app
