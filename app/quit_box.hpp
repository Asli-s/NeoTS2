#pragma once

#include "content/ui_data.hpp"
#include "formats/ui_script.hpp"
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

// The game's quit box, which it builds in code rather than from a UI script: the game's title over
// the question in a framed panel, the buttons below, in the middle of the screen. It asks "Are you
// sure you want to quit?" with Yes and No, or, when there is something to save, "Do you want to
// save before quitting?" with Yes, No and Cancel. Escape answers No or Cancel.
class QuitBox {
public:
    enum class Answer { Yes, No, Cancel };

    // Shows the box over everything on a `width` x `height` screen, asking to save first when
    // `askSave`. Closing it makes the window that was modal before modal again.
    void open(ts2::ui::Screen& screen, ts2::content::UiData& data, int width, int height, bool askSave = false) {
        close(screen);
        modalBefore_ = screen.modal();
        askSave_ = askSave;
        const std::uint32_t questionText = askSave ? kSaveQuestion : kQuestion;
        const std::string title = data.text(kUiText, kTitle), question = data.text(kUiText, questionText);
        struct Choice {
            std::uint32_t id, text;
            int w = 0;
        };
        std::vector<Choice> choices{{kYes, kYesText}, {kNo, kNoText}};
        if (askSave) {
            choices.push_back({kCancel, kCancelText});
        }

        const auto titleSize = screen.textSize(kTitleFont, title);
        // Buttons are their caption plus 16 wide, at least 82, 34 high and 12 apart.
        int buttonsW = -12;
        for (Choice& c : choices) {
            c.w = std::max(82, screen.textSize(kButtonFont, data.text(kUiText, c.text))[0] + 16);
            buttonsW += c.w + 12;
        }
        const int buttonH = 34;
        const auto [bodyW, bodyLines] = bodyLayout(screen, question, std::max({titleSize[0], 128, buttonsW}));
        const int bodyH = bodyLines * screen.textSize(kBodyFont, question)[1];
        const int w = std::max({128, titleSize[0] + 52, buttonsW + 52, bodyW + 52});
        const int bodyY = titleSize[1] + 26;
        const int buttonsY = std::max(bodyY + bodyH + 12, 118 - buttonH);
        const int h = buttonsY + buttonH + 14;

        ts2::ui::Window frame{{{"iid", "IGZWinGen"}, {"paint", "yes"}, {"edgeimage", "yes"}, {"winflag_visible", "yes"},
                               {"area", area(0, 0, w, h)}, {"image", "{499db772,a9500615}"}}, {}};
        frame.children.push_back(text(kTitleFont, kTitle, 21, 13, titleSize[0] + 10, titleSize[1] + 10));
        frame.children.push_back(text(kBodyFont, questionText, 21, bodyY - 5, bodyW + 10, bodyH + 10, bodyLines > 1));
        int x = (w - buttonsW) / 2;
        for (const Choice& c : choices) {
            frame.children.push_back(button(c.id, c.text, x, buttonsY, c.w, buttonH));
            x += c.w + 12;
        }
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

    // Whether it asks to save first (Yes, No and Cancel).
    bool asksSave() const { return askSave_; }

    // A released button's answer, nothing for any other window.
    std::optional<Answer> clicked(const ts2::ui::Screen& screen, int window) const {
        if (root_ < 0 || screen.parent(window) != root_) {
            return std::nullopt;
        }
        switch (screen.id(window)) {
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
    // GenHeader (ITC Benguiat Gothic 18 bold), GenDialogBody (HelveticaNeueLT Std Medium 13) and
    // GenButton (ITC Benguiat Gothic 13 bold). All the box's text is dark blue.
    inline static const std::string kTitleFont = "0x4a809916", kBodyFont = "0x8d2a495c", kButtonFont = "0x4a809919";
    inline static const std::string kTextColor = "(5,6,113)";

    static std::string area(int x, int y, int w, int h) { return std::format("({},{},{},{})", x, y, x + w, y + h); }

    // The question's width and number of lines. It wraps at the widest of `least` (the title, 128
    // and the buttons) and its longest word plus 8. A question that fits on one line is its own
    // width plus 1. A wrapped one taller than 64 widens by 32 at a time, the height allowed rising
    // by 32 each time up to 500, until it fits or is over 700 wide.
    static std::pair<int, int> bodyLayout(ts2::ui::Screen& screen, const std::string& question, int least) {
        // Words end at white space and, as the game measures them, at any byte from 0x80 up.
        int widestWord = 0;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= question.size(); ++i) {
            const auto c = i < question.size() ? static_cast<unsigned char>(question[i]) : ' ';
            if (c >= 0x80 || std::isspace(c) != 0) {
                if (i > start) {
                    widestWord = std::max(widestWord, screen.textSize(kBodyFont, question.substr(start, i - start))[0]);
                }
                start = i + 1;
            }
        }
        const int lineH = screen.textSize(kBodyFont, question)[1];
        int bodyW = std::max(least, widestWord + 8);
        int lines = static_cast<int>(screen.wrapText(kBodyFont, question, bodyW).size());
        for (int room = 64; lines >= 2 && room < lines * lineH && bodyW < 701;) {
            room = std::min(room + 32, 500);
            bodyW += 32;
            lines = static_cast<int>(screen.wrapText(kBodyFont, question, bodyW).size());
        }
        if (lines < 2) {
            return {screen.textSize(kBodyFont, question)[0] + 1, 1};
        }
        return {bodyW, lines};
    }

    static std::string res(std::uint32_t string) { return std::format("{{{:08x},{:08x}}}", kUiText, string); }

    static ts2::ui::Window text(const std::string& font, std::uint32_t string, int x, int y, int w, int h, bool wrapped = false) {
        return {{{"iid", "IGZWinText"}, {"winflag_visible", "yes"}, {"area", area(x, y, w, h)}, {"captionres", res(string)}, {"font", font},
                 {"forecolor", kTextColor}, {"gutters", "(5,5)"}, {"wrapped", wrapped ? "yes" : "no"}},
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
    bool askSave_ = false;
};

} // namespace app
