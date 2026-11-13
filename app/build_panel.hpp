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

// The Build mode panel (UI script 0x49060003): the categories of building tools, and for Floors
// its sub-categories and a catalog of the floors in the one chosen, a page at a time, each a picture
// whose tip gives its name and price per tile. Clicking one picks it up to lay. Only Floors is done:
// the other categories and the tools other than the hand are disabled.
class BuildPanel {
public:
    // `content` is the loaded panel script's root window.
    void attach(ts2::ui::Screen& screen, int content);

    // What the catalog needs: the floors for sale, and a floor's picture (null while it has none).
    using Floor = ts2::scene::Assets::CatalogPattern;
    struct Source {
        const std::vector<Floor>* floors = nullptr;
        std::function<const ts2::tga::Pixels*(const Floor&)> picture;
    };
    // Shows the catalog page for the sub-category chosen (only when something changed since last time).
    void refresh(const Source& source);

    // A click on one of the panel's windows: nothing, a floor to lay (its GUID), the hand tool, or
    // another window of the panel (handled here).
    struct Click {
        bool panel = false;
        std::optional<std::uint32_t> floor;
        bool hand = false;
    };
    Click clicked(int window);

    // Back to the categories, as the panel opens.
    void reset();

    // The floors a floor sub-category lists ("" for all), cheapest first, exposed for tests.
    static std::vector<Floor> listed(const std::vector<Floor>& all, const std::string& subsort);
    // The sub-sort of a floor sub-category button (0xB420 to 0xB490), "" for All (0xB4A0) and others.
    static std::string floorSubsort(std::uint32_t button);

private:
    void showView();

    ts2::ui::Screen* screen_ = nullptr;
    int content_ = -1;
    int categories_ = -1, floorSorts_ = -1, catalog_ = -1;
    int area_ = -1;
    int previous_ = -1, next_ = -1;
    std::vector<int> slots_;
    std::vector<std::uint32_t> slotGuids_;
    ts2::tga::Pixels mask_;
    bool floors_ = false;      // the Floors category is open
    std::uint32_t subsortButton_ = 0;  // the sub-category button chosen, 0: all
    int page_ = 0;
    bool dirty_ = true;
    std::vector<Floor> listed_;
};

} // namespace app
