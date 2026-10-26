#pragma once

#include "content/ui_data.hpp"
#include "sim/pie_menu.hpp"
#include "ui/pie_layout.hpp"
#include "ui/screen.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace app {

// The pie menu on screen, as the game shows it: the choices round the mouse, clockwise from the
// top. A name with '/' opens a sub-menu ("Turn On.../This Light") centred on the item clicked, whose
// centre item goes back. A ring with more than 12 entries pages through them with "More..." (see
// pie::page). The 3D head in the middle is not drawn yet.
class PieMenuUi {
public:
    PieMenuUi(ts2::ui::Screen& screen, ts2::content::UiData& data);
    ~PieMenuUi() { close(); }
    PieMenuUi(const PieMenuUi&) = delete;
    PieMenuUi& operator=(const PieMenuUi&) = delete;

    void open(std::vector<ts2::sim::PieChoice> choices, int x, int y, int screenWidth, int screenHeight);
    void close();
    bool isOpen() const { return root_ >= 0; }
    // The choices offered, in the order they were given.
    const std::vector<ts2::sim::PieChoice>& choices() const { return choices_; }

    enum class Mouse { Move, LeftDown, LeftUp, RightDown };
    // The mouse while the menu is open: moving highlights an item. Releasing the left button after a
    // press on the menu picks the highlighted item. Pressing the left button away from the items, or
    // the right button anywhere, closes the menu.
    std::optional<ts2::sim::PieChoice> mouse(int x, int y, Mouse what);

private:
    struct Node {
        std::string label;
        std::optional<std::size_t> choice;  // a leaf: which choice
        bool disabled = false;
        std::vector<Node> children;
    };
    struct Shown {
        int knob = -1, litKnob = -1, text = -1;
        bool disabled = false;
    };

    // A ring of the menu: its items, the first one on the page shown, its centre and the item it
    // goes back with (sub-menus).
    struct Level {
        const std::vector<Node>* nodes = nullptr;
        std::size_t first = 0;
        int x = 0, y = 0;
        const Node* back = nullptr;
    };
    void show(const Level& level);
    void highlight(std::optional<std::size_t> item, bool back);
    std::optional<ts2::sim::PieChoice> choose(std::size_t item);
    // The entry an item shown stands for. None for "More...".
    std::optional<std::size_t> entryOf(std::size_t item) const;
    int addItem(const ts2::ui::pie::Item& item, int textWidth, std::uint32_t base, const std::string& label, bool disabled, Shown& shown);

    ts2::ui::Screen& screen_;
    std::string moreText_;
    std::vector<ts2::sim::PieChoice> choices_;
    std::vector<Node> top_;
    std::vector<Level> levels_;  // the top ring, then the sub-menus opened
    int screenWidth_ = 0, screenHeight_ = 0;
    int root_ = -1;
    ts2::ui::pie::Layout layout_;
    std::vector<Shown> shown_;
    ts2::ui::pie::Page page_;
    std::optional<ts2::ui::pie::Item> backItem_;
    Shown back_;
    std::optional<std::size_t> highlighted_;
    bool backHighlighted_ = false;
    bool pressed_ = false;  // the left button went down on the open menu
};

} // namespace app
