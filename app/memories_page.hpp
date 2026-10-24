#pragma once

#include "formats/tga.hpp"
#include "ui/screen.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace app {

// Simology's memories page: the Sim's memories as thumbnails on a green (good) or red (bad)
// background, six in a row, or four rows when expanded, turned a page at a time. Clicking one opens
// its popup above the page: its name, the Sim it is about with their portrait, and its description.
class MemoriesPage {
public:
    struct Memory {
        std::string name, description;
        std::string subjectName;  // the Sim it is about (first and last name), empty for none
        bool good = true;
        const ts2::tga::Pixels* picture = nullptr;
        const ts2::tga::Pixels* subjectPicture = nullptr;  // that Sim's portrait

        bool operator==(const Memory&) const = default;
    };

    // `page` is the Simology script's memories window.
    void attach(ts2::ui::Screen& screen, int page);

    // Handles a click on the page's buttons (pages, expand, collapse) or a memory. Returns whether
    // it was one.
    bool clicked(int window);

    // Shows the "maximized" art (wants open) or the "minimized" one.
    void setMaximized(bool maximized);

    // Shows the Sim's memories, the one to show first first.
    void show(std::vector<Memory> memories);

    // The mouse is at (x, y): the popup closes once it leaves the thumbnails (the memory stays
    // highlighted).
    void mouseAt(int x, int y);

private:
    struct Thumbnail {
        int window = -1;
        std::optional<Memory> shown;  // repainted only on a change
    };

    void makeThumbnails(int area, std::size_t rows, std::vector<Thumbnail>& out);
    void layout();
    void showPopup();
    // A window's place within the page.
    std::array<int, 2> placeInPage(int window) const;
    std::array<int, 2> screenPlace(int window) const;

    ts2::ui::Screen* screen_ = nullptr;
    int page_ = -1;
    bool expanded_ = false;
    int pageNumber_ = 0;
    std::vector<Memory> memories_;
    std::vector<Thumbnail> collapsed_, expandedThumbs_;
    int popup_ = -1;
    std::optional<std::size_t> selected_;  // the clicked memory
    bool popupOpen_ = false;
};

} // namespace app
