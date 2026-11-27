#pragma once

#include "catalog_grid.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <vector>

namespace app {

// The Buy mode panel (UI script 0x49060004). Its strip starts with the sort buttons, then the
// chosen sort's buttons, then the catalog of the items they pick. Function sort lists the nine
// categories (Comfort, Surfaces and so on). A category opens its subcategories and the catalog
// of all its items, and a subcategory narrows the catalog to its own. Subcategories with nothing
// in them are greyed. The strip's middle stretches to the chosen buttons and the catalog follows them.
class BuyPanel {
public:
    // A catalog item and the object data fields that sort it: function sort flags (OBJD 0x28),
    // function subsort (OBJD 0x5E), room sort flags (OBJD 0x27) and price (OBJD 0x12).
    struct Entry {
        std::uint32_t guid = 0;
        std::uint16_t functions = 0, subsort = 0, rooms = 0;
        int price = 0;
    };

    // `root` is the loaded panel script's root window.
    void attach(ts2::ui::Screen& screen, int root);
    // The items the catalog can list, in the order it lists them.
    void setCatalog(std::vector<Entry> entries);
    bool hasCatalog() const { return !entries_.empty(); }
    // Shows the panel as Buy mode opens.
    void open();
    // Handles a click on one of the panel's own windows. Returns whether it was one.
    bool clicked(int window);
    // Opens the category and subcategory listing item `guid` (its first function bit and subsort
    // bit), on the page it is on with its cell pressed, as the eyedropper's pick does. False for an
    // item the panel doesn't list.
    bool showItem(std::uint32_t guid);
    CatalogGrid& grid() { return grid_; }

private:
    void layout();
    bool matches(const Entry& entry, int function, int subsort) const;
    bool anyMatch(int function, int subsort) const;

    ts2::ui::Screen* screen_ = nullptr;
    int root_ = -1;
    int sorts_ = -1, categories_ = -1;  // the function sort's container and its category buttons
    std::vector<int> subPanels_;        // a category's subcategory buttons, in the function bits' order
    int function_ = -1;                 // the chosen category's function bit, -1 for none
    // The subcategory chosen in each category (0: all), kept while Buy mode is left.
    std::vector<int> subsorts_;
    std::vector<Entry> entries_;
    CatalogGrid grid_;
};

} // namespace app
