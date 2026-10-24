#pragma once

#include "formats/tga.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace app {

// The live panel's inventory and rewards sub-panel (UI script 0x49064800). Its tabs are career
// rewards, aspiration rewards, the Sim's inventory and the shopping cart. The tab stays chosen
// between openings and goes back to the inventory when the Sim can't use it. Each tab has its own
// background, wider while the wants are open, and the inventory's taller when it is expanded.
class RewardsPanel {
public:
    // A thumbnail: an object's picture, how many are stacked in it, whether it can be had, its
    // price in aspiration points (0: none) and, for career rewards, the career's icon.
    struct Item {
        const ts2::tga::Pixels* picture = nullptr;
        int count = 1;
        bool enabled = true;
        int price = 0;
        const ts2::tga::Pixels* icon = nullptr;
        bool operator==(const Item&) const = default;
    };
    // What the panel shows of the selected Sim.
    struct Sim {
        std::int16_t aspiration = 0;  // its aspiration flag (person data 0x2E, as the aspiration meter reads it)
        bool careerTab = false;       // teens, adults and elders
        bool aspirationTab = false;   // everyone but babies
        bool cartTab = false;         // a shopping token for the lot it is on
        std::vector<Item> inventory;
        int points = 0;               // aspiration reward points
        std::vector<Item> aspirationRewards, careerRewards;
        // With Free Time: whether the lifetime aspiration tree is shown, and whether it has levels to spend.
        bool lifetimeTree = false, lifetimeLevels = false;
        bool operator==(const Sim&) const = default;
    };

    // `content` is the loaded panel script's root window.
    void attach(ts2::ui::Screen& screen, int content);
    // Handles a click on one of the panel's own windows. Returns whether it was one.
    bool clicked(int window);
    // The wants panel is open: the backgrounds are the wider ones.
    void setMaximized(bool maximized);
    void show(const Sim& sim);

private:
    enum Tab { kCareer = 1, kAspiration = 2, kInventory = 3, kCart = 4 };

    // A paged list of thumbnails. The inventory's has a collapsed and an expanded half.
    struct List {
        int group = -1;
        int halves[2] = {-1, -1};   // the second is -1 for lists that don't expand
        std::vector<int> slots[2];  // the thumbnail windows made in each half's area
        std::vector<Item> shown[2];  // what each of them shows
        int page = 0;
        bool expanded = false;
        ts2::tga::Pixels enabledMask, disabledMask;  // the pictures' shapes
    };

    void makeList(List& list, std::uint32_t group, bool expands, std::uint32_t thumbnailScript, std::uint32_t enabledMask,
                  std::uint32_t disabledMask);
    bool listClicked(List& list, std::uint32_t id);
    void selectTab(int tab);
    bool available(int tab) const;
    void layout();
    void layoutList(List& list, const std::vector<Item>& items);
    void showItem(const List& list, int slot, const Item& item, Item& shown);

    ts2::ui::Screen* screen_ = nullptr;
    int content_ = -1;
    int tab_ = kInventory;
    bool maximized_ = false;
    Sim sim_;
    List inventory_, aspiration_, career_;
};

} // namespace app
