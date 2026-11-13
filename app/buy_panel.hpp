#pragma once

#include "formats/tga.hpp"
#include "sim/catalog.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace app {

// The Buy mode panel (UI script 0x49060004): the sorts (by function or by room), the categories and
// their sub-categories, and a catalog of the objects in the one chosen, a page at a time, each a
// picture whose tip gives its name and price. Clicking one buys it. The Community and Collections
// sorts, the expanded catalog and the tools other than the hand aren't done yet: their buttons are
// disabled.
class BuyPanel {
public:
    // `content` is the loaded panel script's root window.
    void attach(ts2::ui::Screen& screen, int content);

    // What the catalog needs from the lot: every object for sale, and an object's picture (null
    // while it has none) and catalog text (line 0 its name).
    struct Source {
        const std::vector<ts2::sim::catalog::Item>* items = nullptr;
        std::function<const ts2::tga::Pixels*(const ts2::sim::catalog::Item&)> picture;
        std::function<std::string(const ts2::sim::catalog::Item&)> name;
    };
    // Shows the catalog page for the category chosen (only when something changed since last time).
    void refresh(const Source& source);

    // A click on one of the panel's windows: nothing, a catalog item to buy (its GUID), or another
    // window of the panel (handled here).
    struct Click {
        bool panel = false;
        std::optional<std::uint32_t> buy;
    };
    Click clicked(int window);

    // Goes back to the sort's first view (the categories, page one), as the panel opens.
    void reset();

    // The objects the catalog lists now (in the chosen category), exposed for tests.
    static std::vector<ts2::sim::catalog::Item> listed(const std::vector<ts2::sim::catalog::Item>& all, bool byRoom,
                                                       std::uint16_t category, std::uint16_t subsort);

private:
    void layout();
    void showSort();

    ts2::ui::Screen* screen_ = nullptr;
    int content_ = -1;
    int area_ = -1;         // the catalog's item area
    int previous_ = -1, next_ = -1;
    std::vector<int> slots_;  // the item windows, a page's worth
    std::vector<std::uint32_t> slotGuids_;  // what each shows now (0: nothing)
    ts2::tga::Pixels mask_;   // the pictures' shape
    bool byRoom_ = false;
    std::uint16_t category_ = 0;  // the function or room bit chosen, 0: none yet (all are listed)
    std::uint32_t categoryButton_ = 0;  // the category button chosen (for its sub-category panel)
    std::uint16_t subsort_ = 0;
    int page_ = 0;
    bool dirty_ = true;
    std::vector<ts2::sim::catalog::Item> listed_;
};

} // namespace app
