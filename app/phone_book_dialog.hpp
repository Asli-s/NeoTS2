#pragma once

#include "formats/tga.hpp"
#include "list_box_rows.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace app {

// The game's Personal Phonebook (Dialog "Phone Invite", UI script 0x8C159244): the Sims a phone call
// can reach as a grid of portraits (cells from script 0x8C159245) or as a list of names, three ways
// of sorting them, the picked Sim's portrait and caption at the top, and OK and Cancel. It opens
// sorted A-Z in the grid with the first Sim picked, as the game's does.
class PhoneBookDialog {
public:
    struct Entry {
        std::int16_t neighbor = 0;
        std::string first, last;
        const ts2::tga::Pixels* picture = nullptr;  // the neighbor's portrait, null while loading
        // The caller's relationship towards the Sim: the marks a cell and a row show, and the
        // relationship sort's key (daily plus lifetime).
        bool friendMark = false, enemy = false, love = false, crush = false;
        int relationship = 0;
        // The chemistry sort's key. Not known yet, so 0 for everyone (see UI-60).
        int chemistry = 0;
    };
    enum class Sort { Name, Relationship, Chemistry };

    // Shows the dialog over everything, in the middle of a `width` x `height` screen (half the room
    // left, rounded down). `caption` is the line over the picked Sim with %s for its name. False
    // when the script is missing.
    bool open(ts2::ui::Screen& screen, int width, int height, const std::string& title, const std::string& caption,
              std::vector<Entry> entries);
    void close(ts2::ui::Screen& screen);
    bool isOpen() const { return root_ >= 0; }
    // Shows the portraits that have finished loading since the dialog opened.
    void updatePictures(ts2::ui::Screen& screen, const std::function<const ts2::tga::Pixels*(std::int16_t)>& pictureOf);
    // A released button or a click on a cell or row. True when the dialog is answered: then
    // `answer` is the neighbor picked, or nothing for Cancel.
    bool clicked(ts2::ui::Screen& screen, int window, std::optional<std::int16_t>& answer);
    // The neighbor picked now, 0 when there is none.
    std::int16_t picked() const;
    // The mouse and the wheel on the list view's scroll bar and rows (see ListBoxRows).
    ListBoxRows& list() { return list_; }

private:
    void sortEntries(ts2::ui::Screen& screen, Sort sort);
    void showView(ts2::ui::Screen& screen, bool grid);
    void showPage(ts2::ui::Screen& screen);
    void showPicked(ts2::ui::Screen& screen);
    void showPicture(ts2::ui::Screen& screen, std::size_t entry);
    void makeCells(ts2::ui::Screen& screen);
    void makeRows(ts2::ui::Screen& screen);

    int root_ = -1;
    int modalBefore_ = -1;
    std::vector<Entry> entries_;  // in the order shown
    std::vector<int> cells_, rows_;  // one per entry, in the order shown
    ListBoxRows list_;
    std::size_t picked_ = 0;
    std::size_t page_ = 0;
    Sort sort_ = Sort::Name;
    bool grid_ = true;
    std::string caption_;
    ts2::tga::Pixels shape_;  // the cell's portrait shape
};

} // namespace app
