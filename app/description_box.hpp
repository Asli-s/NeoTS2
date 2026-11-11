#pragma once

#include "game_scroll_bar.hpp"
#include "platform/platform.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <string>

namespace app {

// A script's editable text box (IGZWinTextEdit), as the neighborhood preview's description and the
// chooser's neighborhood names use them. A wrapped box comes with its upright scroll bar
// (IGZWinScrollbar2), shown only while the text has more lines than the box shows: its value is the
// line at the top. A press on the track above or below the thumb moves a page (the lines shown less
// one), held it repeats every 300 ms while the mouse stays on that part of the track, and the thumb
// follows a drag. The wheel over the text moves a line for each full 100 of its delta.
//
// A press on the text puts the caret there and takes the keyboard. Dragging selects up to the mouse,
// Shift with a press keeps the selection's other end, and a second press within 500 ms selects the
// word under it. Only spaces and line breaks end a word. Shift with a move key extends the
// selection, Ctrl moves by word, or with Home and End to the text's ends. Typing, Backspace and
// Delete replace the selection. The selection and caret show only while the box has the keyboard,
// and the box keeps them when it loses it.
class DescriptionBox {
public:
    // Takes the text box and, for a box that isn't `singleLine`, the scroll bar inside it.
    // `maxCharacters` is the script's maxtext.
    void attach(ts2::ui::Screen& screen, int box, std::size_t maxCharacters, bool singleLine = false);
    // Shows `text` from its first line, with no caret until the box is clicked.
    void setText(ts2::ui::Screen& screen, const std::string& text);
    const std::string& text() const { return text_; }
    bool focused() const { return focused_; }
    void unfocus(ts2::ui::Screen& screen);

    // A left press at screen (x, y) at time `ms`, `shift` when Shift is held: on the bar, or in the
    // box (which then takes the keyboard). True when it was either.
    bool mouseDown(ts2::ui::Screen& screen, int x, int y, std::uint64_t ms, bool shift = false);
    bool holdsMouse() const { return scroll_.holdsMouse() || selecting_; }
    // Every mouse move: drags a held thumb or the selection, otherwise updates which part of the bar
    // is under the mouse.
    void mouseMove(ts2::ui::Screen& screen, int x, int y);
    void mouseUp(ts2::ui::Screen& screen, int x, int y);
    // A wheel turn at screen (x, y), `delta` 120 a notch (positive: away from the player). True
    // when it was over the text.
    bool wheel(ts2::ui::Screen& screen, int x, int y, int delta);
    // Called each frame: repeats a held press on the track.
    void tick(ts2::ui::Screen& screen, std::uint64_t ms);

    void type(ts2::ui::Screen& screen, const std::string& typed);
    void key(ts2::ui::Screen& screen, ts2::platform::Key key, bool shift = false, bool ctrl = false);

private:
    void show(ts2::ui::Screen& screen);
    void showBar(ts2::ui::Screen& screen);
    // The box shows the lines from the bar's value.
    void followBar(ts2::ui::Screen& screen);
    bool inText(const ts2::ui::Screen& screen, int x, int y) const;
    // Takes out the selection. False when there is none.
    bool eraseSelection();
    // Selects the word at the caret, or the word just before a space the caret is on.
    void selectWord();
    // The ends of the line the caret is on, without its line break.
    std::size_t lineStart(const ts2::ui::Screen& screen) const;
    std::size_t lineEnd(const ts2::ui::Screen& screen) const;

    int box_ = -1;
    GameScrollBar scroll_;
    std::size_t maxCharacters_ = 0;
    bool singleLine_ = false;
    std::string text_;
    std::size_t caret_ = 0, anchor_ = 0;  // in bytes, the selection runs from the anchor to the caret
    bool focused_ = false;
    bool selecting_ = false;  // a press on the text is held
    // When the last press on the text was, while a second one would select a word.
    std::uint64_t pressedAt_ = 0;
    bool timing_ = false;
};

} // namespace app
