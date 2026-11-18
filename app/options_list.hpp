#pragma once

#include "list_box_rows.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace app {

// A list box of the options panels: rows with a radio dot, one of them picked, and the game's list
// box scroll bar (see ListBoxRows). The script holds only an empty window for the list, and the
// game builds the rest in code.
class OptionsList {
public:
    // Each row's button: its size (a width of 0 is the list's), gutters and caption alignment, and
    // how the rows are laid out (see ListBoxRows::Layout). The default is the options panels' rows,
    // 17 high: the height of their radio dot picture plus 2.
    struct RowStyle {
        ListBoxRows::Layout layout{17};
        int width = 0, height = 17;
        std::string gutters = "(4,2,4,2)", align = "center";
        // A row script (as the game's song rows, UI script 0x49060F04) to build each row from instead:
        // its window 1 is the radio dot, 4 the caption, 5 and 6 the hovered and picked bars, 7 the
        // caption's width while picked, and 2 and 3 the play and stop buttons. A click on the caption
        // picks the row and one on the dot turns it on or off. The pointer shows the hovered bar.
        std::uint32_t scriptGroup = 0, scriptInstance = 0;
    };

    // `list` is the script's empty window. Its width and height are the rows' area.
    // `image` is the rows' picture as a script writes it, "{group,instance}"; empty for the radio dots.
    void attach(ts2::ui::Screen& screen, int list, const std::string& image = {});
    void attach(ts2::ui::Screen& screen, int list, const RowStyle& style);
    void setRows(const std::vector<std::string>& rows);
    // With `multi`, each row is a box that a click turns on or off and any number may be on, not one
    // pick among them. The boxes are given by setChecks.
    void setMulti(bool multi) { multi_ = multi; }
    void setChecks(const std::vector<bool>& checks);
    const std::vector<bool>& checks() const { return checks_; }
    // The row the last click turned on or off, -1 before any.
    int toggled() const { return toggled_; }
    // Picks a row, -1 for none. A row below the ones shown scrolls up to be the last one shown.
    void pick(int row);
    int picked() const { return picked_; }
    void setEnabled(bool enabled);

    // A click on one of its rows picks it. True when the window is one of them.
    bool clicked(int window);

    // The mouse and the wheel on the list's bar and rows (see ListBoxRows).
    bool mouseDown(int x, int y, std::uint64_t ms) { return box_.mouseDown(x, y, ms); }
    void mouseMove(int x, int y);
    void mouseUp(int x, int y) { box_.mouseUp(x, y); }
    bool holdsMouse() const { return box_.holdsMouse(); }
    void tick(std::uint64_t ms) { box_.tick(ms); }
    bool wheel(int x, int y, int delta) { return box_.wheel(x, y, delta); }

private:
    void showChecks();
    void showPicked();

    // A row built from the row script, by its windows (see RowStyle).
    struct ScriptRow {
        int dot = -1, caption = -1, play = -1, stop = -1, hovered = -1, picked = -1;
        int captionWidth = 0, pickedWidth = 0;
    };
    std::vector<ScriptRow> scriptRows_;
    ts2::ui::Screen* screen_ = nullptr;
    int list_ = -1;
    ListBoxRows box_;
    std::vector<int> rows_;
    int picked_ = -1;
    bool enabled_ = true;
    bool multi_ = false;
    std::vector<bool> checks_;
    int toggled_ = -1;
    std::string image_;
    RowStyle style_;
};

} // namespace app
