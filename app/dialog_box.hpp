#pragma once

#include "ui/screen.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
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

    // Shows the box over everything on a `width` x `height` screen, with `picture` (framed, 44 x 44)
    // left of the text when given. Closing it makes the window that was modal before modal again.
    void open(ts2::ui::Screen& screen, int width, int height, const std::string& title, const std::string& message,
              const std::vector<Button>& buttons, const ts2::tga::Pixels* picture = nullptr) {
        close(screen);
        const std::string body = lineBreaks(message);
        modalBefore_ = screen.modal();
        const auto titleSize = screen.textSize(kTitleFont, title);
        std::vector<int> buttonW;
        for (const Button& b : buttons) {
            // The game makes a button its caption plus 16 wide, then lays the box out with that plus 16.
            buttonW.push_back(screen.textSize(kButtonFont, b.caption)[0] + 32);
        }
        const int buttonsW = buttonWidths(buttonW);
        const int buttonH = 34;
        const int picW = picture != nullptr ? static_cast<int>(picture->width) : 0;
        const int picH = picture != nullptr ? static_cast<int>(picture->height) : 0;
        const int lineH = screen.textSize(kBodyFont, body)[1];
        const auto [bodyW, bodyLines] = bodyLayout(screen, body, std::max({titleSize[0], 128, buttonsW}), picW);
        int w = std::max({128, buttonsW + 52, picture != nullptr ? picW + 60 + bodyW : bodyW + 52});
        if (!title.empty()) {
            w = std::max(w, titleSize[0] + 52);
        }
        // Without a title the text starts higher. With a picture it starts right of it.
        const int bodyX = picture != nullptr ? picW + 34 : 26;
        const int bodyY = title.empty() ? 18 : titleSize[1] + 26;
        // A text with more than two lines past those beside the picture goes on below it, across the box.
        const int besidePicture = picture != nullptr && lineH > 0 ? (picH + 4) / lineH + 1 : 0;
        std::string first = body, rest;
        int firstLines = bodyLines, restLines = 0;
        if (besidePicture > 0 && bodyLines > besidePicture + 2) {
            std::tie(first, rest) = splitAfterLines(screen, body, bodyW, besidePicture);
            firstLines = besidePicture;
            restLines = static_cast<int>(screen.wrapText(kBodyFont, rest, w - 57).size());
        }
        const int firstH = firstLines * lineH;
        const int textBottom = restLines > 0 ? bodyY + firstH + restLines * lineH : bodyY + std::max(firstH, picH);
        const int buttonsY = std::max(textBottom + 12, 118 - buttonH);
        const int h = buttonsY + buttonH + 14;

        ts2::ui::Window frame{{{"iid", "IGZWinGen"}, {"paint", "yes"}, {"edgeimage", "yes"}, {"winflag_visible", "yes"},
                               {"area", area(0, 0, w, h)}, {"image", "{499db772,a9500615}"}}, {}};
        if (!title.empty()) {
            frame.children.push_back(text(kTitleFont, title, 21, 13, titleSize[0] + 10, titleSize[1] + 10));
        }
        if (picture != nullptr) {
            frame.children.push_back({{{"iid", "IGZWinBMP"}, {"id", std::format("0x{:08x}", kPictureId)}, {"winflag_visible", "yes"},
                                       {"area", area(26, title.empty() ? 20 : titleSize[1] + 28, picW, picH)}},
                                      {}});
        }
        frame.children.push_back(text(kBodyFont, first, bodyX - 5, bodyY - 5, bodyW + 10, firstH + 10, firstLines > 1));
        if (restLines > 0) {
            frame.children.push_back(text(kBodyFont, rest, 21, bodyY + firstH - 5, w - 42, restLines * lineH + 10, true, false));
        }
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
        if (picture != nullptr) {
            screen.setImage(screen.find(root_, kPictureId), *picture);
        }
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

    // Script texts end their lines with \r, \r\n or \n. The box breaks lines at \n.
    static std::string lineBreaks(std::string text) {
        for (std::size_t at = text.find('\r'); at != std::string::npos; at = text.find('\r', at)) {
            text.replace(at, at + 1 < text.size() && text[at + 1] == '\n' ? 2 : 1, "\n");
        }
        return text;
    }

    static std::string area(int x, int y, int w, int h) { return std::format("({},{},{},{})", x, y, x + w, y + h); }

    // Buttons are 34 high and 12 apart, and their total width is returned. From the widest down,
    // the buttons less than 20 narrower than a width are made as wide. A width under 82 is raised to
    // 82 and takes every narrower button with it.
    static int buttonWidths(std::vector<int>& widths) {
        int below = std::numeric_limits<int>::max();
        while (true) {
            int widest = 0;
            for (const int bw : widths) {
                if (bw > widest && bw < below) {
                    widest = bw;
                }
            }
            if (widest == 0) {
                break;
            }
            int least = widest - 20;
            if (widest < 82) {
                widest = 82;
                least = 0;
            }
            for (int& bw : widths) {
                if (bw < widest && bw > least) {
                    bw = widest;
                }
            }
            below = widest;
        }
        int total = -12;
        for (const int bw : widths) {
            total += bw + 12;
        }
        return std::max(total, 0);
    }

    // The text up to the end of its first `lines` lines wrapped at `width`, and the rest after them.
    static std::pair<std::string, std::string> splitAfterLines(ts2::ui::Screen& screen, const std::string& body, int width, int lines) {
        const auto wrapped = screen.wrapText(kBodyFont, body, width);
        std::size_t at = 0;
        for (int i = 0; i < lines && i < static_cast<int>(wrapped.size()); ++i) {
            std::size_t word = 0;
            const std::string& line = wrapped[static_cast<std::size_t>(i)];
            while (word < line.size()) {
                while (word < line.size() && line[word] == ' ') {
                    ++word;
                }
                const std::size_t end = std::min(line.find(' ', word), line.size());
                const std::size_t found = body.find(line.substr(word, end - word), at);
                if (found == std::string::npos) {
                    break;
                }
                at = found + (end - word);
                word = end;
            }
        }
        std::size_t restStart = at;
        while (restStart < body.size() && std::isspace(static_cast<unsigned char>(body[restStart])) != 0) {
            ++restStart;
        }
        return {body.substr(0, at), body.substr(restStart)};
    }

    // The text's width and number of lines. It wraps at the widest of `least` (the title, 128 and
    // the buttons) and its longest word plus 8, less the picture's width `picW` and 8 (at least 128)
    // when there is one. A text that fits on one line is its own width plus 1. A wrapped one taller
    // than 64 widens by 32 at a time, the height allowed rising by 32 each time up to 500, until it
    // fits or is over 700 wide.
    static std::pair<int, int> bodyLayout(ts2::ui::Screen& screen, const std::string& body, int least, int picW) {
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
        if (picW > 0) {
            bodyW = std::max(128, bodyW - picW - 8);
        }
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
    static constexpr std::uint32_t kPictureId = 0xB0D2;  // the picture's id in the box

    static ts2::ui::Window text(const std::string& font, const std::string& caption, int x, int y, int w, int h, bool wrapped = false,
                                bool body = true) {
        return {{{"iid", "IGZWinText"}, {"id", font == kBodyFont && body ? std::format("0x{:08x}", kBodyId) : "0"}, {"winflag_visible", "yes"}, {"area", area(x, y, w, h)}, {"caption", caption}, {"font", font},
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
