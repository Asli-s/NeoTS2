#pragma once

#include "platform/platform.hpp"
#include "ui/screen.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace app {

// A text box the player types into (a script's IGZWinTextEdit): its text, its caret while it has the
// keyboard, and what typing and the editing keys do to it. Text is UTF-8; the caret moves by
// characters. `multiline` boxes take Return as a new line; others let the keyboard go on Return.
class TextField {
public:
    TextField() = default;
    TextField(int window, std::size_t maxBytes, bool multiline) : window_(window), maxBytes_(maxBytes), multiline_(multiline) {}

    int window() const { return window_; }
    const std::string& text() const { return text_; }
    void setText(ts2::ui::Screen& screen, std::string text);
    bool focused() const { return focused_; }
    // Takes or lets go of the keyboard (the caret goes to the end when taken).
    void focus(ts2::ui::Screen& screen, bool focused);
    // Typed text at the caret. Whether the text changed.
    bool typed(ts2::ui::Screen& screen, const std::string& text);
    // An editing key (Backspace, Delete, arrows, Home, End, Return, Escape). Whether the text changed.
    bool key(ts2::ui::Screen& screen, ts2::platform::Key key);
    // Whether `window` is this box or inside it.
    bool holds(const ts2::ui::Screen& screen, int window) const;

private:
    void show(ts2::ui::Screen& screen) const;

    int window_ = -1;
    std::size_t maxBytes_ = 256;
    bool multiline_ = false;
    std::string text_;
    std::size_t caret_ = 0;
    bool focused_ = false;
};

} // namespace app
