#pragma once

#include "meter_bar.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace app {

// Simology's interests page: each conversation topic's name over a bar of the Sim's interest in it
// (person data 0x7C to 0x8D), three in a row, three rows or six when expanded, turned a page at a
// time.
class InterestsPage {
public:
    // `page` is the Simology script's interests window.
    void attach(ts2::ui::Screen& screen, int page);

    // Handles a click on the page's buttons (pages, expand, collapse). Returns whether it was one.
    bool clicked(int window);

    // Shows the "maximized" art (wants open) or the "minimized" one.
    void setMaximized(bool maximized);

    // Shows a Sim's person data.
    void show(std::span<const std::int16_t> personData);

private:
    struct Item {
        int window = -1;
        MeterBar bar;
        int topic = -1;  // the topic it shows, by its place in the list
    };

    void makeItems(int area, int rows, std::vector<Item>& out);
    void layout();

    ts2::ui::Screen* screen_ = nullptr;
    int page_ = -1;
    bool expanded_ = false;
    int pageNumber_ = 0;
    std::vector<std::int16_t> values_;  // per topic
    std::vector<Item> collapsed_, expandedItems_;
};

} // namespace app
