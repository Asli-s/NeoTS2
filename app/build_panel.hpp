#pragma once

#include "formats/tga.hpp"
#include "scene/assets.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace app {

// The Build mode panel (UI script 0x49060003): the categories of building tools, and for Floors and
// Wall Coverings their sub-categories and a catalog of the patterns in the one chosen, a page at a
// time, each a picture whose tip gives its name and price per tile. Clicking one picks it up to lay.
// Only those two categories are done: the others and the tools other than the hand are disabled.
class BuildPanel {
public:
    // `content` is the loaded panel script's root window.
    void attach(ts2::ui::Screen& screen, int content);

    // What the catalog needs: the floors and wall coverings for sale, and a pattern's picture (null
    // while it has none).
    using Pattern = ts2::scene::Assets::CatalogPattern;
    struct Source {
        const std::vector<Pattern>* floors = nullptr;
        const std::vector<Pattern>* walls = nullptr;
        // The doors, windows and arches, as patterns with no material (their sub-sorts doors,
        // tallwindows, windows, arches or multistory).
        const std::vector<Pattern>* objects = nullptr;
        std::function<const ts2::tga::Pixels*(const Pattern&)> picture;
    };
    // Shows the catalog page for the sub-category chosen (only when something changed since last time).
    void refresh(const Source& source);

    // A click on one of the panel's windows: nothing, a floor or wall covering to lay (its GUID),
    // the hand tool, or another window of the panel (handled here).
    struct Click {
        bool panel = false;
        std::optional<std::uint32_t> floor, wall, object;  // object: a door, window or arch
        bool hand = false;
        bool wallTool = false;  // the Wall tool (Walls & Rooms)
    };
    Click clicked(int window);

    // Back to the categories, as the panel opens.
    void reset();

    // The patterns a sub-category lists ("" for all), in the catalog's order, exposed for tests.
    static std::vector<Pattern> listed(const std::vector<Pattern>& all, const std::string& subsort);
    // The sub-sort of a floor (0xB420 to 0xB490) or wall covering (0xB520 to 0xB5A1) sub-category
    // button, "" for All (0xB4A0, 0xB5A0) and others.
    static std::string subsortOf(std::uint32_t button);

private:
    enum class Category { None, Floors, Walls, WallTool, Doors };
    void showView();

    ts2::ui::Screen* screen_ = nullptr;
    int content_ = -1;
    int categories_ = -1, floorSorts_ = -1, wallSorts_ = -1, wallToolSorts_ = -1, doorSorts_ = -1, catalog_ = -1;
    int area_ = -1;
    int previous_ = -1, next_ = -1;
    std::vector<int> slots_;
    std::vector<std::uint32_t> slotGuids_;
    ts2::tga::Pixels mask_;
    Category category_ = Category::None;
    std::uint32_t subsortButton_ = 0;  // the sub-category button chosen, 0: all
    int page_ = 0;
    bool dirty_ = true;
    bool wallToolPicked_ = false;  // Walls & Rooms shows pressed
    std::vector<Pattern> listed_;
};

} // namespace app
