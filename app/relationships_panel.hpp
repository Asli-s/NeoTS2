#pragma once

#include "formats/tga.hpp"
#include "ui/screen.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace app {

// The live panel's relationships: a page of thumbnails of the Sims the selected Sim knows (six in a
// row, or two rows when expanded), each with its daily and lifetime meters and icons, picked by a
// filter (household, family, friends, chemistry or all) and turned a page at a time.
class RelationshipsPanel {
public:
    // A Sim the selected Sim knows, as the panel's list holds it.
    struct Person {
        std::int16_t neighbor = 0;
        int daily = 0, lifetime = 0;     // relationship values 0 and 2
        std::int16_t flags = 0;          // relationship value 1 (bit 15: listed)
        int chemistry = 0;               // 1..7
        int chemistryKey = 0;            // the chemistry filter's order (highest first)
        float enemy = 0, contact = 0;    // how far along the enemy or contact overlays are
        bool family = false, household = false, pet = false, female = false;
        bool coins = false;              // the gold coins icon (what sets it is not read)
        bool skip = false;               // never listed
        int sortGroup = 0;               // listed first when lower (before lifetime)
        std::string sortName, sortName2;  // then by these
        const ts2::tga::Pixels* picture = nullptr;

        bool operator==(const Person&) const = default;
    };

    // `content` is the loaded relationships script's root window.
    void attach(ts2::ui::Screen& screen, int content);

    // Handles a click on the panel's buttons (filters, pages, expand). Returns whether it was one.
    bool clicked(int window);

    // Shows the "maximized" art (wants open) or the "minimized" one.
    void setMaximized(bool maximized);

    // Shows the selected Sim's list. `species` is the selected Sim's (person data 0xBA, 0 a person).
    void show(std::vector<Person> people, std::int16_t species);

private:
    struct Thumbnail {
        int window = -1;
        int daily = -1, lifetime = -1;          // the meters' fills
        int dailyRow = -1, lifetimeRow = -1;    // the gradient rows they show
        std::optional<Person> shown;  // what it shows, so it is repainted only on a change
        int shownFilter = 0;
        std::int16_t shownSpecies = 0;
    };

    void makeThumbnails(int area, std::size_t count, std::vector<Thumbnail>& out);
    void showThumbnail(Thumbnail& t, const Person& p, std::int16_t species);
    void showMeter(int fill, int value, int& shownRow, const ts2::tga::Pixels& fillPicture);
    void showFrame(int window, const ts2::tga::Pixels& strip, int frame, int frameWidth);
    void layout();

    ts2::ui::Screen* screen_ = nullptr;
    int content_ = -1;
    int filter_ = 5;        // 1 household, 2 family, 3 friends, 4 chemistry, 5 all
    bool expanded_ = false; // two rows
    int page_ = 0;
    std::vector<Person> listed_;
    std::int16_t species_ = 0;
    std::vector<Thumbnail> collapsed_, expandedThumbs_;
    ts2::tga::Pixels gradient_, dailyFill_, lifetimeFill_, icons_, enemies_, contacts_;
};

} // namespace app
