#pragma once

#include "content/ui_data.hpp"
#include "formats/ui_script.hpp"
#include "ui/screen.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace app {

// The game's "Are you sure you want to quit?" box, which it builds in code rather than from a UI
// script: the game's title over the question in a framed panel, Yes and No below, in the middle of
// the screen. Escape answers No.
class QuitBox {
public:
    // Shows the box over everything on a `width` x `height` screen. Closing it gives the window that
    // was modal before back its place.
    void open(ts2::ui::Screen& screen, ts2::content::UiData& data, int width, int height) {
        close(screen);
        modalBefore_ = screen.modal();
        const std::string title = data.text(kUiText, kTitle), question = data.text(kUiText, kQuestion);
        const std::string yes = data.text(kUiText, kYesText), no = data.text(kUiText, kNoText);

        const auto titleSize = screen.textSize(kTitleFont, title);
        const auto bodySize = screen.textSize(kBodyFont, question);
        // Buttons are their caption plus 16 wide, at least 82, 34 high and 12 apart.
        const int yesW = std::max(82, screen.textSize(kButtonFont, yes)[0] + 16);
        const int noW = std::max(82, screen.textSize(kButtonFont, no)[0] + 16);
        const int buttonsW = yesW + 12 + noW, buttonH = 34;
        const int bodyW = bodySize[0] + 1;
        const int w = std::max({128, titleSize[0] + 52, buttonsW + 52, bodyW + 52});
        const int bodyY = titleSize[1] + 26;
        const int buttonsY = std::max(bodyY + bodySize[1] + 12, 118 - buttonH);
        const int h = buttonsY + buttonH + 14;

        ts2::ui::Window frame{{{"iid", "IGZWinGen"}, {"paint", "yes"}, {"edgeimage", "yes"}, {"winflag_visible", "yes"},
                               {"area", area(0, 0, w, h)}, {"image", "{499db772,a9500615}"}}, {}};
        frame.children.push_back(text(kTitleFont, kTitle, 21, 13, titleSize[0] + 10, titleSize[1] + 10));
        frame.children.push_back(text(kBodyFont, kQuestion, 21, bodyY - 5, bodyW + 10, bodySize[1] + 10));
        const int x = (w - buttonsW) / 2;
        frame.children.push_back(button(kYes, kYesText, x, buttonsY, yesW, buttonH));
        frame.children.push_back(button(kNo, kNoText, x + yesW + 12, buttonsY, noW, buttonH));
        root_ = screen.addWindows({frame}, (width - w) / 2, (height - h) / 2);
        screen.raise(root_);
        screen.setModal(root_);
    }

    void close(ts2::ui::Screen& screen) {
        if (root_ >= 0) {
            screen.remove(root_);
            screen.setModal(modalBefore_);
            root_ = -1;
            modalBefore_ = -1;
        }
    }

    bool isOpen() const { return root_ >= 0; }

    // A released button: true for Yes, false for No, nothing for any other window.
    std::optional<bool> clicked(const ts2::ui::Screen& screen, int window) const {
        if (root_ < 0 || screen.parent(window) != root_) {
            return std::nullopt;
        }
        const std::uint32_t id = screen.id(window);
        return id == kYes ? std::optional<bool>(true) : id == kNo ? std::optional<bool>(false) : std::nullopt;
    }

private:
    // UIText strings, by string table and 1-based entry: the title (the product titles' entry for
    // the game itself, Mansion and Garden Stuff), the question, Yes and No.
    static constexpr std::uint32_t kUiText = 0x7FA7DC7C;
    static constexpr std::uint32_t kTitle = 0x00010012, kQuestion = 0x00990003, kYesText = 0x008E0003, kNoText = 0x008E0004;  // string ids, leakcheck: ok
    // The game's Yes and No button ids.
    static constexpr std::uint32_t kYes = 0x5301814C, kNo = 0x53018149;
    // GenHeader (ITC Benguiat Gothic 18 bold), GenDialogBody (HelveticaNeueLT Std Medium 13) and
    // GenButton (ITC Benguiat Gothic 13 bold). All the box's text is dark blue.
    inline static const std::string kTitleFont = "0x4a809916", kBodyFont = "0x8d2a495c", kButtonFont = "0x4a809919";
    inline static const std::string kTextColor = "(5,6,113)";

    static std::string area(int x, int y, int w, int h) { return std::format("({},{},{},{})", x, y, x + w, y + h); }

    static std::string res(std::uint32_t string) { return std::format("{{{:08x},{:08x}}}", kUiText, string); }

    static ts2::ui::Window text(const std::string& font, std::uint32_t string, int x, int y, int w, int h) {
        return {{{"iid", "IGZWinText"}, {"winflag_visible", "yes"}, {"area", area(x, y, w, h)}, {"captionres", res(string)}, {"font", font},
                 {"forecolor", kTextColor}, {"gutters", "(5,5)"}},
                {}};
    }

    static ts2::ui::Window button(std::uint32_t id, std::uint32_t string, int x, int y, int w, int h) {
        return {{{"iid", "IGZWinBtn"}, {"winflag_visible", "yes"}, {"id", std::format("0x{:08x}", id)}, {"area", area(x, y, w, h)},
                 {"captionres", res(string)}, {"showcaption", "yes"}, {"font", kButtonFont}, {"colorfontnormal", kTextColor}, {"colorfontdisabled", kTextColor},
                 {"align", "center"}, {"image", "{499db772,a9500630}"}},
                {}};
    }

    int root_ = -1;
    int modalBefore_ = -1;
};

} // namespace app
