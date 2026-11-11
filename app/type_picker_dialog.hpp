#pragma once

#include "formats/tga.hpp"
#include "list_box_rows.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace app {

// The game's object type picker (Dialog type 0x34, UI script 0x8C159243, "Call Sim Options" after a
// call's Sim is picked): a title, a caption beside an icon, and a list of object types by catalog
// name, sorted A-Z with the first picked, then OK and Cancel.
class TypePickerDialog {
public:
    struct Option {
        std::uint32_t guid = 0;
        std::string name;  // its catalog name
    };

    // Shows the dialog over everything, in the middle of a `width` x `height` screen. `icon` replaces
    // the script's picture beside the caption when given. False when the script is missing.
    bool open(ts2::ui::Screen& screen, int width, int height, const std::string& title, const std::string& caption,
              std::vector<Option> options, const ts2::tga::Pixels* icon);
    void close(ts2::ui::Screen& screen);
    bool isOpen() const { return root_ >= 0; }
    // A released button or a click on a row. True when the dialog is answered: then `answer` is the
    // GUID picked, or nothing for Cancel.
    bool clicked(ts2::ui::Screen& screen, int window, std::optional<std::uint32_t>& answer);
    // The mouse and the wheel on the list's scroll bar and rows (see ListBoxRows).
    ListBoxRows& list() { return list_; }

private:
    void showPicked(ts2::ui::Screen& screen);

    int root_ = -1;
    int modalBefore_ = -1;
    std::vector<Option> options_;  // in the order shown
    std::vector<int> rows_;        // one per option
    ListBoxRows list_;
    std::size_t picked_ = 0;
};

} // namespace app
