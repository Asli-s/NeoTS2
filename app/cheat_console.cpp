#include "cheat_console.hpp"

#include "diag/log.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>

namespace app {

namespace {

const std::string kFont = "DefaultFont13";
// The game's window. Closed, a light panel 24 high across the screen's top holds a white field 16
// high with a dark 1-pixel edge, 4 in from the sides and the top. With the output open the panel is
// 200 high and the field 180 from the top. Above it the output runs from 4 in to 9 in from the right
// and from 4 to 175 down. It shows 13 rows 13 apart, 7 in and 2 down, and the scroll bar, 16 wide,
// stands 8 in from the screen's right over the output's height.
constexpr int kBarHeight = 24, kOpenHeight = 200, kFieldX = 4, kFieldY = 4, kOpenFieldY = 180, kFieldHeight = 16;
constexpr int kOutputX = 4, kOutputY = 4, kOutputRight = 9, kOutputBottom = 175;
constexpr int kLineHeight = 13, kPaneLines = 13, kTextX = 7, kTextY = 2;
constexpr int kScrollWidth = ts2::ui::ScrollBar::kCell, kScrollRight = 8, kScrollTop = kOutputY, kScrollBottom = kOutputBottom;
// The game's scroll bar picture: a row of 16-pixel cells, those across the screen first, then the
// upright ones: up arrow, up arrow pressed, thumb, track, down arrow, down arrow pressed.
const std::string kScrollImage = "{499db772,00001003}";
constexpr int kUpCell = 96, kUpPressedCell = 112, kThumbCell = 128, kTrackCell = 144, kDownCell = 160, kDownPressedCell = 176;
// The window's one font: Lucida Console at 10 pixels, a character 6 wide as in the game's window.
// Without it the Default style.
const std::string kOutputFont = "CheatConsole";
// Our own limits, so typing or pasting can't grow the line, the output or the history without end.
// How much history the game keeps by default isn't known yet.
constexpr std::size_t kMaxLine = 1024, kMaxOutput = 200, kMaxHistory = 100;

bool continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

std::size_t previous(const std::string& text, std::size_t at) {
    while (at > 0 && continuation(text[--at])) {
    }
    return at;
}

std::size_t next(const std::string& text, std::size_t at) {
    if (at >= text.size()) {
        return text.size();
    }
    while (++at < text.size() && continuation(text[at])) {
    }
    return at;
}

ts2::ui::Window window(const char* iid, int x, int y, int w, int h, const std::string& font = kFont) {
    ts2::ui::Window out;
    out.attributes = {{"iid", iid}, {"area", std::format("({},{},{},{})", x, y, x + w, y + h)}, {"winflag_visible", "yes"}};
    if (std::string_view(iid) == "IGZWinText") {
        out.attributes.emplace_back("font", font);
    } else {
        out.attributes.emplace_back("paint", "yes");  // a generic window draws its image only with paint
    }
    return out;
}

// One pixel of a colour, stretched over a window's area.
ts2::tga::Pixels shade(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    return {1, 1, {r, g, b, a}};
}

bool startsWithNoCase(std::string_view text, std::string_view start) {
    return text.size() >= start.size() && std::equal(start.begin(), start.end(), text.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
}

} // namespace

void CheatConsole::build(ts2::ui::Screen& screen, int width) {
    width_ = width;
    bar_ = screen.addWindows({window("IGZWinGen", 0, 0, width, kBarHeight)}, 0, 0);
    screen.setImage(bar_, shade(236, 235, 248, 255));
    const int fieldW = width - 2 * kFieldX;
    edge_ = screen.addWindows({window("IGZWinGen", kFieldX, kFieldY, fieldW, kFieldHeight)}, kFieldX, kFieldY, bar_);
    screen.setImage(edge_, shade(52, 52, 60, 255));
    const int field = screen.addWindows({window("IGZWinGen", 1, 1, fieldW - 2, kFieldHeight - 2)}, 1, 1, edge_);
    screen.setImage(field, shade(255, 255, 255, 255));
    lineText_ = screen.addWindows({window("IGZWinText", 2, 1, fieldW - 6, kFieldHeight - 2, kOutputFont)}, 2, 1, field);
    screen.setTextColor(lineText_, {0, 0, 0, 1});
    pane_ = screen.addWindows({window("IGZWinGen", kOutputX, kOutputY, width - kOutputX - kOutputRight, kOutputBottom - kOutputY)}, kOutputX, kOutputY, bar_);
    screen.setImage(pane_, shade(232, 233, 255, 255));
    paneLines_.clear();
    for (int row = 0; row < kPaneLines; ++row) {
        const int y = kTextY + row * kLineHeight;
        const auto line = window("IGZWinText", kTextX, y, width - kOutputX - kTextX - kScrollWidth - kScrollRight - 4, kLineHeight, kOutputFont);
        paneLines_.push_back(screen.addWindows({line}, kTextX, y, pane_));
        screen.setTextColor(paneLines_.back(), {0, 0, 0, 1});
    }
    const int scrollX = width - kScrollRight - kScrollWidth;
    const int scrollH = kScrollBottom - kScrollTop;
    scroll_.setHeight(scrollH);
    const auto cell = [&](int y, int h) {
        // The cells are inside the output, placed from its corner.
        auto w = window("IGZWinGen", scrollX - kOutputX, kScrollTop - kOutputY + y, kScrollWidth, h);
        w.attributes.emplace_back("image", kScrollImage);
        return screen.addWindows({w}, scrollX - kOutputX, kScrollTop - kOutputY + y, pane_);
    };
    const int cellSize = ts2::ui::ScrollBar::kCell;
    scrollTrack_ = cell(cellSize, scrollH - 2 * cellSize);
    screen.setImagePart(scrollTrack_, {kTrackCell, 0, kTrackCell + cellSize, cellSize});
    scrollUp_ = cell(0, cellSize);
    scrollDown_ = cell(scrollH - cellSize, cellSize);
    scrollThumb_ = cell(0, cellSize);
    screen.setImagePart(scrollThumb_, {kThumbCell, 0, kThumbCell + cellSize, cellSize});
    screen.setVisible(bar_, false);
}

void CheatConsole::layout(ts2::ui::Screen& screen) {
    screen.resize(bar_, width_, expanded_ ? kOpenHeight : kBarHeight);
    const int fieldY = expanded_ ? kOpenFieldY : kFieldY;
    screen.move(edge_, kFieldX, fieldY);
    fieldArea_ = {kFieldX, fieldY, width_ - 2 * kFieldX, kFieldHeight};
    screen.setVisible(pane_, expanded_);
}

bool CheatConsole::scroll(ts2::ui::Screen& screen, int x, int y, int lines) {
    if (!open_ || !expanded_ || y < 0 || y >= kOpenFieldY || x < 0 || x >= width_) {
        return false;
    }
    scroll_.setPosition(scroll_.position() - lines);
    showOutput(screen);
    return true;
}

void CheatConsole::toggle(ts2::ui::Screen& screen, int width) {
    if (open_) {
        close(screen);
        return;
    }
    if (bar_ < 0 || width != width_) {
        if (bar_ >= 0) {
            screen.remove(bar_);
        }
        build(screen, width);
    }
    open_ = true;
    layout(screen);
    screen.setVisible(bar_, true);
    screen.raise(bar_);
    focus(screen, true);
    showOutput(screen);
}

void CheatConsole::close(ts2::ui::Screen& screen) {
    if (!open_) {
        return;
    }
    focus(screen, false);
    open_ = false;
    screen.setVisible(bar_, false);
}

void CheatConsole::focus(ts2::ui::Screen& screen, bool on) {
    if (on != focused_) {
        focused_ = on;
        if (on) {
            ts2::platform::startTextInput();
        } else {
            ts2::platform::stopTextInput();
        }
    }
    showLine(screen);
}

bool CheatConsole::mouseDown(ts2::ui::Screen& screen, int x, int y, bool left, std::uint64_t ms) {
    if (!open_) {
        return false;
    }
    const auto [fx, fy, fw, fh] = fieldArea_;
    focus(screen, x >= fx && x < fx + fw && y >= fy && y < fy + fh);
    if (left && onScrollBar(x, y)) {
        scroll_.press(scrollY(y), ms);
        showOutput(screen);
    }
    return y < (expanded_ ? kOpenHeight : kBarHeight);
}

void CheatConsole::mouseMove(ts2::ui::Screen& screen, int x, int y) {
    if (scroll_.move(scrollY(y), onScrollBar(x, y))) {
        showOutput(screen);
    }
}

void CheatConsole::mouseUp(ts2::ui::Screen& screen, int, int y) {
    scroll_.release(scrollY(y));
    showOutput(screen);
}

void CheatConsole::tick(ts2::ui::Screen& screen, std::uint64_t ms) {
    if (scroll_.tick(ms)) {
        showOutput(screen);
    }
}

bool CheatConsole::onScrollBar(int x, int y) const {
    const int scrollX = width_ - kScrollRight - kScrollWidth;
    return open_ && expanded_ && x >= scrollX && x < scrollX + kScrollWidth && y >= kScrollTop && y < kScrollBottom;
}

int CheatConsole::scrollY(int y) const {
    return y - kScrollTop;
}

void CheatConsole::text(ts2::ui::Screen& screen, const std::string& typed) {
    if (!focused_ || line_.size() + typed.size() > kMaxLine) {
        return;
    }
    line_.insert(caret_, typed);
    caret_ += typed.size();
    showLine(screen);
}

void CheatConsole::key(ts2::ui::Screen& screen, ts2::platform::Key key, ts2::sim::CheatLot* lot) {
    using ts2::platform::Key;
    if (!focused_) {
        return;
    }
    switch (key) {
    case Key::Return: run(screen, lot); return;
    case Key::Escape: close(screen); return;
    case Key::Backspace:
        if (caret_ > 0) {
            const std::size_t from = previous(line_, caret_);
            line_.erase(from, caret_ - from);
            caret_ = from;
        }
        break;
    case Key::Delete:
        if (caret_ < line_.size()) {
            line_.erase(caret_, next(line_, caret_) - caret_);
        }
        break;
    case Key::Left: caret_ = previous(line_, caret_); break;
    case Key::Right: caret_ = next(line_, caret_); break;
    case Key::Home: caret_ = 0; break;
    case Key::End: caret_ = line_.size(); break;
    // Up shows the line run before the one shown. Down the one after it, and past the last an empty line.
    case Key::Up:
        if (historyAt_ == 0) {
            return;
        }
        line_ = history_[--historyAt_];
        caret_ = line_.size();
        break;
    case Key::Down:
        if (historyAt_ + 1 < history_.size()) {
            line_ = history_[++historyAt_];
        } else if (historyAt_ + 1 == history_.size()) {
            historyAt_ = history_.size();
            line_.clear();
        } else {
            return;
        }
        caret_ = line_.size();
        break;
    default: return;
    }
    showLine(screen);
}

void CheatConsole::run(ts2::ui::Screen& screen, ts2::sim::CheatLot* lot) {
    const bool wasExpanded = expanded_;
    const std::string line = line_;
    addOutput(">" + line);
    const auto result = ts2::sim::runCheat(properties_, lot, line);
    ts2::diag::step(std::format("cheat \"{}\": {}", line, result.ok ? "done" : "failed"));
    for (const auto& text : result.output) {
        addOutput(text);
    }
    switch (result.window) {
    case ts2::sim::CheatResult::Window::Expand: expanded_ = !expanded_; break;
    case ts2::sim::CheatResult::Window::Clear:
        output_.clear();
        scroll_.setLines(1, kPaneLines);
        scroll_.setPosition(0);
        break;
    case ts2::sim::CheatResult::Window::Hide: break;
    case ts2::sim::CheatResult::Window::None: break;
    }
    if (!expanded_ && (!result.ok || startsWithNoCase(line, "help"))) {
        expanded_ = true;
    }
    // A line the same as the last one run isn't kept twice.
    if (!line.empty() && (history_.empty() || history_.back() != line)) {
        if (history_.size() >= kMaxHistory) {
            history_.erase(history_.begin());
        }
        history_.push_back(line);
    }
    historyAt_ = history_.size();
    line_.clear();
    caret_ = 0;
    if (result.window == ts2::sim::CheatResult::Window::Hide || (!expanded_ && !wasExpanded)) {
        close(screen);
        return;
    }
    layout(screen);
    showLine(screen);
    showOutput(screen);
}

void CheatConsole::showLine(ts2::ui::Screen& screen) {
    screen.setText(lineText_, line_);
    screen.setCaret(lineText_, focused_ ? std::optional<std::size_t>(caret_) : std::nullopt);
}

void CheatConsole::showOutput(ts2::ui::Screen& screen) {
    // The lines that fit, from the scroll bar's position.
    const std::size_t from = static_cast<std::size_t>(scroll_.position());
    for (std::size_t row = 0; row < paneLines_.size(); ++row) {
        const std::size_t at = from + row;
        screen.setText(paneLines_[row], at < output_.size() ? output_[at] : std::string());
    }
    showScrollBar(screen);
}

void CheatConsole::showScrollBar(ts2::ui::Screen& screen) {
    using Part = ts2::ui::ScrollBar::Part;
    const int cellSize = ts2::ui::ScrollBar::kCell;
    const int up = scroll_.pressed() == Part::Up ? kUpPressedCell : kUpCell;
    const int down = scroll_.pressed() == Part::Down ? kDownPressedCell : kDownCell;
    screen.setImagePart(scrollUp_, {up, 0, up + cellSize, cellSize});
    screen.setImagePart(scrollDown_, {down, 0, down + cellSize, cellSize});
    screen.move(scrollThumb_, width_ - kScrollRight - kScrollWidth - kOutputX, kScrollTop - kOutputY + scroll_.thumbTop());
}

void CheatConsole::addOutput(std::string line) {
    if (output_.size() >= kMaxOutput) {
        output_.pop_front();
    }
    output_.push_back(std::move(line));
    // New output shows its end. Each output line ends with a line break, so the game's text holds one
    // empty line more, and the last row shows it.
    scroll_.setLines(static_cast<int>(output_.size()) + 1, kPaneLines);
    scroll_.setPosition(scroll_.lastPosition());
}

} // namespace app
