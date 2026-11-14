#pragma once

#include "text_field.hpp"

#include "content/ui_data.hpp"
#include "formats/tga.hpp"
#include "platform/platform.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace app {

// Create-a-Sim (UI scripts 0xCB980E50 and on, group 0x08000600), as Create New Family opens it: the
// screen laid out as the game lays it out for 800 x 600, in the middle of a larger screen, over a
// backdrop. Its wizard's steps each show a panel beside the step buttons:
// - Step 1 (Name / Genetics, 0xCB980E81): the Sim's first name, gender, age and skin tone, and
//   Randomize, which draws another Sim of the same.
// - Step 3 (Hair & Face, 0xCB980E82): hair styles of the chosen hair colour (black, brown, blonde,
//   red), set for every outfit.
// - Step 5 (Clothing, 0xCB980E84): tops, bottoms and outfits for each outfit category.
// A step's choices (hair styles, clothes) are listed by the game side (see setChoices), in a grid of
// six a page with their pictures. The Sim is shown as a picture the game side draws (see
// setPreview), turned with the puck's rotate buttons.
//
// Not built yet: steps 2, 4 and 6 (heads, makeup, personality), the face tabs, custom hair colours,
// hair per outfit, the Sim bin, biography, custom skins and fitness, collections, the family's view
// (its name and members), family relationships, and accepting (saving) the family. Their buttons
// are greyed.
class CasScreen {
public:
    CasScreen(ts2::ui::Screen& screen, ts2::content::UiData& data) : screen_(screen), data_(data) {}

    // The kinds of clothes the Clothing step lists.
    enum class Clothes { Tops, Bottoms, Outfits };
    // The Sim being made: its names and biography, age flag (see kAdultAge...), gender (1 female,
    // 2 male), skin tone (1 to 4), the draw that picks the rest of its look, its hair colour (1 to 4),
    // and the outfit category (1 everyday, 4 formal...) and kind of clothes the Clothing step shows.
    struct Sim {
        std::string firstName, bio;
        std::uint32_t age = 0x08;
        std::uint32_t gender = 1;
        int skin = 1;
        std::uint32_t seed = 1;
        int hairColour = 2;
        std::uint32_t category = 1;
        Clothes clothes = Clothes::Outfits;
    };
    enum class Step { Genetics, Hair, Clothing };

    // Shows it on a `width` x `height` screen with a new Sim drawn by `seed`.
    void open(int width, int height, std::uint32_t seed);
    void close();
    bool isOpen() const { return root_ >= 0; }

    // Remake: the Sim is drawn anew (age, gender, skin, Randomize). Recolour: its hair colour changed.
    // ChoicesNeeded: the step or its tab changed, so its choices are listed again. Chosen: a choice
    // was clicked (see chosen()). Turned: the camera turned. Category: the Clothing step's outfit
    // category changed (the Sim is shown in it).
    enum class Action { None, Cancel, Remake, Recolour, ChoicesNeeded, Chosen, Turned, Category };
    // A released button of the screen.
    Action clicked(int window);
    void pointerDown(int window);
    bool editing() const;
    void typed(const std::string& text);
    void key(ts2::platform::Key key);

    const Sim& sim() const { return sim_; }
    Step step() const { return step_; }
    const std::string& familyName() const { return family_.text(); }
    // The picture of the Sim, drawn for the preview's size and the turn below.
    void setPreview(const ts2::tga::Pixels& picture);
    int previewWidth() const;
    int previewHeight() const;
    // How far the Sim is turned from facing the camera, in degrees.
    float turn() const { return turn_; }

    // The step's choices: how many, and which one the Sim has (marked). Their pictures come after.
    void setChoices(std::size_t count, std::optional<std::size_t> worn);
    // The choices on the page shown, which want pictures.
    std::vector<std::size_t> shownChoices() const;
    void setChoicePicture(std::size_t choice, const ts2::tga::Pixels& picture);
    // The size a choice's picture is drawn at.
    int choiceWidth() const { return choiceWidth_; }
    int choiceHeight() const { return choiceHeight_; }
    std::size_t chosen() const { return chosen_; }

private:
    struct Tile {
        int button = -1, picture = -1;
        std::size_t choice = 0;
    };
    void showStep(Step step);
    void showSim();
    void layoutChoices();
    int find(std::uint32_t id) const;

    ts2::ui::Screen& screen_;
    ts2::content::UiData& data_;
    int backdrop_ = -1, root_ = -1, panel_ = -1, preview_ = -1;
    Step step_ = Step::Genetics;
    TextField first_, family_, bio_;
    Sim sim_;
    float turn_ = 0;
    // The choices' grid: its window, the page's first choice, their count, the one worn, the tiles.
    int grid_ = -1;
    std::size_t firstChoice_ = 0, choices_ = 0, chosen_ = 0;
    std::optional<std::size_t> worn_;
    std::vector<Tile> tiles_;
    std::vector<ts2::tga::Pixels> pictures_;
    int choiceWidth_ = 96, choiceHeight_ = 128;
};

} // namespace app
