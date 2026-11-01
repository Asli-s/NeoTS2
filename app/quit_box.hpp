#pragma once

#include "content/ui_data.hpp"
#include "dialog_box.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace app {

// The game's quit box, a dialog box with the game's title. It asks "Are you sure you want to quit?"
// with Yes and No, or, when there is something to save, "Do you want to save before quitting?" with
// Yes, No and Cancel. Escape answers No or Cancel.
class QuitBox {
public:
    enum class Answer { Yes, No, Cancel };

    // Shows the box over everything on a `width` x `height` screen, asking to save first when
    // `askSave`. Closing it makes the window that was modal before modal again.
    void open(ts2::ui::Screen& screen, ts2::content::UiData& data, int width, int height, bool askSave = false) {
        askSave_ = askSave;
        std::vector<DialogBox::Button> buttons{{kYes, data.text(kUiText, kYesText)}, {kNo, data.text(kUiText, kNoText)}};
        if (askSave) {
            buttons.push_back({kCancel, data.text(kUiText, kCancelText)});
        }
        box_.open(screen, width, height, data.text(kUiText, kTitle), data.text(kUiText, askSave ? kSaveQuestion : kQuestion), buttons);
    }

    void close(ts2::ui::Screen& screen) { box_.close(screen); }
    bool isOpen() const { return box_.isOpen(); }

    // Whether it asks to save first (Yes, No and Cancel).
    bool asksSave() const { return askSave_; }

    // A released button's answer, nothing for any other window.
    std::optional<Answer> clicked(const ts2::ui::Screen& screen, int window) const {
        switch (box_.clicked(screen, window).value_or(0)) {
        case kYes: return Answer::Yes;
        case kNo: return Answer::No;
        case kCancel: return Answer::Cancel;
        default: return std::nullopt;
        }
    }

private:
    // UIText strings, by string table and 1-based entry: the title (the product titles' entry for
    // the game itself, Mansion and Garden Stuff), the two questions, Yes, No and Cancel.
    static constexpr std::uint32_t kUiText = 0x7FA7DC7C;
    static constexpr std::uint32_t kTitle = 0x00010012, kQuestion = 0x00990003, kSaveQuestion = 0x00990007;  // string ids, leakcheck: ok
    static constexpr std::uint32_t kYesText = 0x008E0003, kNoText = 0x008E0004, kCancelText = 0x008E0002;    // string ids, leakcheck: ok
    // The game's Yes, No and Cancel button ids.
    static constexpr std::uint32_t kYes = 0x5301814C, kNo = 0x53018149, kCancel = 0x53018147;

    DialogBox box_;
    bool askSave_ = false;
};

} // namespace app
