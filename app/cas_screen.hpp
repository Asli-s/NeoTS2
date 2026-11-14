#pragma once

#include "text_field.hpp"

#include "content/ui_data.hpp"
#include "formats/tga.hpp"
#include "platform/platform.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <string>

namespace app {

// Create-a-Sim (UI scripts 0xCB980E50 and on, group 0x08000600), as Create New Family opens it: the
// screen laid out as the game lays it out for 800 x 600, in the middle of a larger screen, over a
// backdrop. Step 1 (Name / Genetics, 0xCB980E81) sets the Sim's first name, biography, gender, age
// and skin tone, and Randomize draws another Sim of the same.
// The Sim is shown as a picture the game side draws (see setPreview), turned with the puck's rotate
// buttons.
//
// Not built yet: steps 2 to 6 (heads, hair and face, makeup, clothing, personality), the Sim bin,
// custom skins and fitness, the family's view (its name and members), family relationships, and accepting (saving) the
// family. Their buttons are greyed.
class CasScreen {
public:
    CasScreen(ts2::ui::Screen& screen, ts2::content::UiData& data) : screen_(screen), data_(data) {}

    // The Sim being made: its names and biography, age flag (see kAdultAge...), gender (1 female,
    // 2 male), skin tone (1 to 4) and the draw that picks the rest of its look.
    struct Sim {
        std::string firstName, bio;
        std::uint32_t age = 0x08;
        std::uint32_t gender = 1;
        int skin = 1;
        std::uint32_t seed = 1;
    };

    // Shows it on a `width` x `height` screen with a new Sim drawn by `seed`.
    void open(int width, int height, std::uint32_t seed);
    void close();
    bool isOpen() const { return root_ >= 0; }

    enum class Action { None, Cancel, LookChanged, Turned };
    // A released button of the screen.
    Action clicked(int window);
    void pointerDown(int window);
    bool editing() const;
    void typed(const std::string& text);
    void key(ts2::platform::Key key);

    const Sim& sim() const { return sim_; }
    const std::string& familyName() const { return family_.text(); }
    // The picture of the Sim, drawn for the preview's size and the turn below.
    void setPreview(const ts2::tga::Pixels& picture);
    int previewWidth() const;
    int previewHeight() const;
    // How far the Sim is turned from facing the camera, in degrees.
    float turn() const { return turn_; }

private:
    void showSim();
    int find(std::uint32_t id) const;

    ts2::ui::Screen& screen_;
    ts2::content::UiData& data_;
    int backdrop_ = -1, root_ = -1, step_ = -1, preview_ = -1;
    TextField first_, family_, bio_;
    Sim sim_;
    float turn_ = 0;
};

} // namespace app
