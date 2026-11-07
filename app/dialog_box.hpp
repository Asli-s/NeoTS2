#pragma once

#include "ui/screen.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace app {

// The dialog box the game builds in code rather than from a UI script: a title over a text in a
// framed panel, the buttons below, in the middle of the screen. The quit box and the object error
// box are such boxes.
class DialogBox {
public:
    struct Button {
        std::uint32_t id;
        std::string caption;
    };

    // Shows the box over everything on a `width` x `height` screen. Closing it makes the window that
    // was modal before modal again.
    void open(ts2::ui::Screen& screen, int width, int height, const std::string& title, const std::string& body,
              const std::vector<Button>& buttons) {
        close(screen);
        modalBefore_ = screen.modal();
        const auto titleSize = screen.textSize(kTitleFont, title);
        // Buttons are their caption plus 16 wide, at least 82, 34 high and 12 apart.
        std::vector<int> buttonW;
        int buttonsW = -12;
        for (const Button& b : buttons) {
            buttonW.push_back(std::max(82, screen.textSize(kButtonFont, b.caption)[0] + 16));
            buttonsW += buttonW.back() + 12;
        }
        const int buttonH = 34;
        const auto [bodyW, bodyLines] = bodyLayout(screen, body, std::max({titleSize[0], 128, buttonsW}));
        const int bodyH = bodyLines * screen.textSize(kBodyFont, body)[1];
        const int w = std::max({128, titleSize[0] + 52, buttonsW + 52, bodyW + 52});
        const int bodyY = titleSize[1] + 26;
        const int buttonsY = std::max(bodyY + bodyH + 12, 118 - buttonH);
        const int h = buttonsY + buttonH + 14;

        ts2::ui::Window frame{{{"iid", "IGZWinGen"}, {"paint", "yes"}, {"edgeimage", "yes"}, {"winflag_visible", "yes"},
                               {"area", area(0, 0, w, h)}, {"image", "{499db772,a9500615}"}}, {}};
        frame.children.push_back(text(kTitleFont, title, 21, 13, titleSize[0] + 10, titleSize[1] + 10));
        frame.children.push_back(text(kBodyFont, body, 21, bodyY - 5, bodyW + 10, bodyH + 10, bodyLines > 1));
        int x = (w - buttonsW) / 2;
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            frame.children.push_back(button(buttons[i].id, x, buttonsY, buttonW[i], buttonH));
            x += buttonW[i] + 12;
        }
        buttons_.clear();
        for (const Button& b : buttons) {
            buttons_.push_back(b.id);
        }
        root_ = screen.addWindows({frame}, (width - w) / 2, (height - h) / 2);
        body_ = screen.find(root_, kBodyId);
        // A button takes its caption from a text resource only, so the texts are set once it is made.
        for (const Button& b : buttons) {
            if (const int made = screen.find(root_, b.id); made >= 0) {
                screen.setText(made, b.caption);
            }
        }
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

    // Puts the open box back in the middle of a `width` x `height` screen.
    void centre(ts2::ui::Screen& screen, int width, int height) {
        if (root_ >= 0) {
            screen.move(root_, (width - screen.width(root_)) / 2, (height - screen.height(root_)) / 2);
        }
    }

    // Changes the text of the open box, which keeps its size.
    void setBody(ts2::ui::Screen& screen, const std::string& body) {
        if (root_ >= 0) {
            screen.setText(body_, body);
        }
    }

    // The id of a released button, nothing for any other window.
    std::optional<std::uint32_t> clicked(const ts2::ui::Screen& screen, int window) const {
        if (root_ < 0 || screen.parent(window) != root_ || std::ranges::find(buttons_, screen.id(window)) == buttons_.end()) {
            return std::nullopt;
        }
        return screen.id(window);
    }

private:
    // GenHeader (ITC Benguiat Gothic 18 bold), GenDialogBody (HelveticaNeueLT Std Medium 13) and
    // GenButton (ITC Benguiat Gothic 13 bold). All the box's text is dark blue.
    inline static const std::string kTitleFont = "0x4a809916", kBodyFont = "0x8d2a495c", kButtonFont = "0x4a809919";
    inline static const std::string kTextColor = "(5,6,113)";

    static std::string area(int x, int y, int w, int h) { return std::format("({},{},{},{})", x, y, x + w, y + h); }

    // The text's width and number of lines. It wraps at the widest of `least` (the title, 128 and
    // the buttons) and its longest word plus 8. A text that fits on one line is its own width plus 1.
    // A wrapped one taller than 64 widens by 32 at a time, the height allowed rising by 32 each time
    // up to 500, until it fits or is over 700 wide.
    static std::pair<int, int> bodyLayout(ts2::ui::Screen& screen, const std::string& body, int least) {
        // Words end at white space and, as the game measures them, at any byte from 0x80 up.
        int widestWord = 0;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= body.size(); ++i) {
            const auto c = i < body.size() ? static_cast<unsigned char>(body[i]) : ' ';
            if (c >= 0x80 || std::isspace(c) != 0) {
                if (i > start) {
                    widestWord = std::max(widestWord, screen.textSize(kBodyFont, body.substr(start, i - start))[0]);
                }
                start = i + 1;
            }
        }
        const int lineH = screen.textSize(kBodyFont, body)[1];
        int bodyW = std::max(least, widestWord + 8);
        int lines = static_cast<int>(screen.wrapText(kBodyFont, body, bodyW).size());
        for (int room = 64; lines >= 2 && room < lines * lineH && bodyW < 701;) {
            room = std::min(room + 32, 500);
            bodyW += 32;
            lines = static_cast<int>(screen.wrapText(kBodyFont, body, bodyW).size());
        }
        if (lines < 2) {
            return {screen.textSize(kBodyFont, body)[0] + 1, 1};
        }
        return {bodyW, lines};
    }

    // The body text's id in the box.
    static constexpr std::uint32_t kBodyId = 0xB0D1;

    static ts2::ui::Window text(const std::string& font, const std::string& caption, int x, int y, int w, int h, bool wrapped = false) {
        return {{{"iid", "IGZWinText"}, {"id", font == kBodyFont ? std::format("0x{:08x}", kBodyId) : "0"}, {"winflag_visible", "yes"}, {"area", area(x, y, w, h)}, {"caption", caption}, {"font", font},
                 {"forecolor", kTextColor}, {"gutters", "(5,5)"}, {"wrapped", wrapped ? "yes" : "no"}},
                {}};
    }

    static ts2::ui::Window button(std::uint32_t id, int x, int y, int w, int h) {
        return {{{"iid", "IGZWinBtn"}, {"winflag_visible", "yes"}, {"id", std::format("0x{:08x}", id)}, {"area", area(x, y, w, h)},
                 {"showcaption", "yes"}, {"font", kButtonFont}, {"colorfontnormal", kTextColor}, {"colorfontdisabled", kTextColor},
                 {"align", "center"}, {"image", "{499db772,a9500630}"}},
                {}};
    }

    int root_ = -1;
    int body_ = -1;
    int modalBefore_ = -1;
    std::vector<std::uint32_t> buttons_;  // the buttons' ids
};

} // namespace app
