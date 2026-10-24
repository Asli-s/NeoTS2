#pragma once

#include "content/ui_queue.hpp"
#include "formats/tga.hpp"
#include "formats/ui_script.hpp"
#include "ui/screen.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace app {

// The live-mode queue at the top left of the screen: an icon for each interaction of the selected
// Sim that has one, the running one first (lit) and then the waiting ones. A cancelled running
// interaction is crossed out until it ends. Clicking an icon cancels its interaction.
class QueuePanel {
public:
    // Reads the row's place and the icon script. No icons until show().
    void attach(ts2::ui::Screen& screen);

    struct Item {
        std::uint32_t id = 0;
        // At the icon's picture size (iconSize()). Without one the icon shows its frame only.
        const ts2::tga::Pixels* picture = nullptr;
        bool running = false, cancelled = false, nested = false;
        std::string tip;
    };
    // Shows these icons, in this order. Icons are kept by id, so an interaction keeps its icon as
    // the queue moves on.
    void show(const std::vector<Item>& items);
    void setVisible(bool visible);
    // The icon under the mouse shows its highlight.
    void mouseAt(int x, int y);

    // The size of an icon's picture (the shape it is cut to).
    std::uint32_t iconSize() const { return topShape_.width; }
    // The id of the interaction whose icon is at (x, y) on the screen, if any.
    std::optional<std::uint32_t> itemAt(int x, int y) const;

private:
    // One icon: both of the script's frames, of which one shows.
    struct Icon {
        int top = -1, nested = -1;
        std::uint32_t id = 0;
        const ts2::tga::Pixels* picture = nullptr;
        bool pictureSet = false;
        bool shown = false;
        bool isNested = false;
        // Where it is and where it slides to. A new interaction's icon starts at its place.
        bool placed = false;
        float x = 0, y = 0;
        int targetX = 0, targetY = 0;
        std::chrono::steady_clock::time_point stepped;
    };
    Icon& iconFor(std::uint32_t id);
    void fill(Icon& icon, const Item& item, const ts2::content::QueueSlot& slot);
    void slide(Icon& icon, std::chrono::steady_clock::time_point now);

    ts2::ui::Screen* screen_ = nullptr;
    ts2::content::QueueLayout layout_;
    // The icon script's kAnimationSteps and kAnimationDelta (the game's own values without them).
    int slideSteps_ = 6;
    float slideDelta_ = 1;
    std::vector<ts2::ui::Window> top_, nested_;  // the script's two frames
    ts2::tga::Pixels topShape_, nestedShape_;  // the pictures' shapes in each frame
    std::vector<Icon> icons_;
    bool visible_ = true;
};

} // namespace app
