#pragma once

#include "chemistry_page.hpp"
#include "hobbies_page.hpp"
#include "interests_page.hpp"
#include "memories_page.hpp"
#include "personality_page.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace app {

// The live panel's Simology: its tab buttons each open one page (personality, interests, hobbies,
// memories, chemistry).
class SimologyPanel {
public:
    // `content` is the loaded Simology script's root window.
    void attach(ts2::ui::Screen& screen, int content);

    // Handles a click on one of the tab buttons. Returns whether it was one.
    bool clicked(int window);

    // Whether the memories page is open (its list is worth making), and showing it.
    bool showsMemories() const { return page_ == kMemoriesTab; }
    void showMemories(std::vector<MemoriesPage::Memory> memories) { memories_.show(std::move(memories)); }
    // Shows the "maximized" art (wants open) or the "minimized" one.
    void setMaximized(bool maximized) {
        interests_.setMaximized(maximized);
        hobbies_.setMaximized(maximized);
        memories_.setMaximized(maximized);
    }
    void mouseMoved(int x, int y) {
        if (showsMemories()) {
            memories_.mouseAt(x, y);
        }
    }

    // Shows a Sim: its life stage (person data 0x3A), person data and the hobbies whose Secret Lot
    // it has found.
    void show(std::int16_t lifeStage, std::span<const std::int16_t> personData, std::span<const std::int16_t> secretLots);

private:
    void showPage(std::uint32_t button);

    ts2::ui::Screen* screen_ = nullptr;
    int content_ = -1;
    std::uint32_t page_ = 0;  // the pressed tab
    bool chemistry_ = true;   // whether its tab can be pressed
    PersonalityPage personality_;
    InterestsPage interests_;
    HobbiesPage hobbies_;
    ChemistryPage chemistryPage_;
    MemoriesPage memories_;
    static constexpr std::uint32_t kMemoriesTab = 0x60000000;
};

} // namespace app
