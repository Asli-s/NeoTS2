#pragma once

#include "catalog_grid.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace app {

// The Build mode panel (UI script 0x49060003). Its top level lists the build categories (walls,
// doors and windows, flooring and so on). A category opens its own buttons, with a back button to
// the top level, and the catalog of the button chosen. Object catalogs list the items whose build
// type (OBJD 0x45) and build subsort (OBJD 0x4A) have the button's bits. Wall and floor catalogs
// list the coverings of the button's subsort. The wall tool lists its own entries, and 1/2 Walls the
// half walls, and Fireplaces the mantles of the catalog scripts' fireplaces. Roofs, terrain, fences
// and the other tools aren't listed yet.
class BuildPanel {
public:
    // The wall tool category's Walls & Rooms button, which picks the wall tool.
    static constexpr std::uint32_t kWallsAndRoomsButton = 0x0000B120;
    // A catalog item and the object data fields that sort it.
    struct Entry {
        std::uint32_t guid = 0;
        std::uint16_t types = 0, subsort = 0;
    };
    // A wall or floor covering and its subsort's name.
    struct CoveringEntry {
        std::uint32_t guid = 0;
        bool wall = false;
        std::string subsort;
    };
    // What the catalog shown lists: objects, wall coverings, floor coverings, the wall tool's own
    // entries or half walls.
    enum class Listing { Objects, Walls, Floors, Tools, HalfWalls };

    // `root` is the loaded panel script's root window.
    void attach(ts2::ui::Screen& screen, int root);
    // The items the catalogs can list, in the order they list them.
    void setCatalog(std::vector<Entry> entries);
    bool hasCatalog() const { return !entries_.empty(); }
    // The wall and floor coverings the catalogs can list, in the order they list them.
    void setCoverings(std::vector<CoveringEntry> coverings);
    bool hasCoverings() const { return !coverings_.empty(); }
    // The wall tool's catalog entries, by the ids the grid shows them under (never 0).
    void setTools(std::vector<std::uint32_t> tools);
    bool hasTools() const { return !tools_.empty(); }
    // The half walls' GUIDs, in the order 1/2 Walls lists them.
    void setHalfWalls(std::vector<std::uint32_t> guids);
    // The fireplaces' mantle GUIDs, in the order Fireplaces lists them.
    void setFireplaces(std::vector<std::uint32_t> mantles);
    bool hasFireplaces() const { return !fireplaces_.empty(); }
    Listing listing() const { return listing_; }
    // Shows the panel as Build mode opens.
    void open();
    // Opens Flooring or Wall Coverings on the sub-category listing covering `guid`, its cell pressed,
    // as the eyedropper's pick of a floor tile or wall does.
    void showCovering(std::uint32_t guid, bool wall);
    // Opens the category and sub-category listing object `guid` (the first button whose build type
    // and subsort bits it has), on its page with its cell pressed. False for an item not listed.
    bool showItem(std::uint32_t guid);
    // Handles a click on one of the panel's own windows. Returns whether it was one.
    bool clicked(int window);
    CatalogGrid& grid() { return grid_; }
    bool wallCategoryOpen() const { return category_ == kWallCategory; }
    bool floorCategoryOpen() const { return category_ == kFloorCategory; }
    bool wallCoveringCategoryOpen() const { return category_ == kWallCoveringCategory; }
    // Whether opening the wall tool's category, or clicking Walls & Rooms or 1/2 Walls, picked the wall
    // tool since the last call.
    bool takeWallPicked() { return std::exchange(wallPicked_, false); }
    // Shows the button of the catalog shown (Walls & Rooms or 1/2 Walls) pressed while the wall tool is
    // in use.
    void setWallTool(bool on);

private:
    static constexpr int kWallCategory = 0;
    static constexpr int kFloorCategory = 3;
    static constexpr int kWallCoveringCategory = 4;

    static constexpr int kMiscCategory = 9;
    static constexpr std::uint32_t kFireplacesButton = 0x0000BA30;
    void layout();

    ts2::ui::Screen* screen_ = nullptr;
    int root_ = -1;
    int sorts_ = -1, categories_ = -1;
    std::vector<int> subPanels_;  // each category's buttons, in the category buttons' order
    int category_ = -1;           // the category open, -1 for the top level
    // The catalog button chosen in each category (0: none), kept while Build mode is left.
    std::vector<std::uint32_t> chosen_;
    std::vector<Entry> entries_;
    std::vector<CoveringEntry> coverings_;
    std::vector<std::uint32_t> tools_;  // see setTools
    std::vector<std::uint32_t> halfWalls_;
    Listing listing_ = Listing::Objects;
    std::vector<std::uint32_t> fireplaces_;
    bool wallPicked_ = false;  // see takeWallPicked
    CatalogGrid grid_;
};

} // namespace app
