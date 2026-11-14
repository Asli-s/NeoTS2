#pragma once

#include "dialog_box.hpp"
#include "meter_bar.hpp"
#include "text_field.hpp"

#include "content/ui_data.hpp"
#include "formats/tga.hpp"
#include "platform/platform.hpp"
#include "ui/screen.hpp"

#include <array>
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
// - Step 6 (Personality, 0xCB980E83): the five personality traits (25 points to share, set from a
//   zodiac sign or bar by bar) and the aspiration (Grow Up for toddlers and children).
// A step's choices (hair styles, clothes) are listed by the game side (see setChoices), in a grid of
// six a page with their pictures. The Sim is shown as a picture the game side draws (see
// setPreview), turned with the puck's rotate buttons.
//
// It opens on a new Sim. DONE adds the Sim to the family (when it has a first name, all its
// personality points and an aspiration, as the game asks) and shows the family's view: its name,
// its members standing together (the game side draws them, see setFamilyPicture), Create a Sim
// for another (up to 8), a member clicked to edit it again (its Cancel Sim asks whether to delete
// it), and Accept Family and Cancel Family. Accept Family asks for the family's name and an adult or
// elder, as the game does.
//
// Not built yet: steps 2 and 4 (heads, makeup), turn-ons and offs, the face tabs, custom hair colours,
// hair per outfit, the Sim bin, biography, custom skins and fitness, collections, family
// relationships, and saving the family. Their buttons are greyed.
class CasScreen {
public:
    // The game's data is reached through the screen.
    explicit CasScreen(ts2::ui::Screen& screen) : screen_(screen) {}

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
        // Personality points (0 to 10): neat, outgoing, active, playful, nice, as the bars stand;
        // the zodiac sign they point to (1 Aries to 12 Pisces); the aspiration (0 none chosen, 1
        // romance, 2 family, 3 fortune, 5 popularity, 6 knowledge, 7 grow up, 8 pleasure).
        std::array<int, 5> traits{};
        int zodiac = 1;
        int aspiration = 0;
    };
    enum class Step { Genetics, Hair, Clothing, Personality };
    // The points a Sim has to share between its traits.
    static constexpr int kPersonalityPoints = 25;

    // Shows it on a `width` x `height` screen with a new Sim drawn by `seed`.
    void open(int width, int height, std::uint32_t seed);
    void close();
    bool isOpen() const { return root_ >= 0; }

    // Remake: the Sim is drawn anew (age, gender, skin, Randomize). Recolour: its hair colour changed.
    // ChoicesNeeded: the step or its tab changed, so its choices are listed again. Chosen: a choice
    // was clicked (see chosen()). Turned: the camera turned. Category: the Clothing step's outfit
    // category changed (the Sim is shown in it).
    // The family: NewSim, a new Sim is made (drawn anew). EditSim: member() is edited again (its look
    // comes back). SimDone: the Sim made or edited is member() now. SimDropped: the Sim made or
    // edited is let go (a member edited stays as it was). MemberDeleted: member() was taken out.
    // Each of the last four shows the family's view, which wants drawing again. Accepted: Accept
    // Family's checks passed, and the screen is to close.
    enum class Action {
        None,
        Cancel,
        Remake,
        Recolour,
        ChoicesNeeded,
        Chosen,
        Turned,
        Category,
        NewSim,
        EditSim,
        SimDone,
        SimDropped,
        MemberDeleted,
        Accepted
    };
    // A released button of the screen.
    Action clicked(int window);
    // The mouse went down on `window`, at (x, y) on the screen.
    Action pointerDown(int window, int x, int y);
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

    // The family: its members (as made), whether its view shows (else a Sim is being made), the
    // member the last action was about.
    const std::vector<Sim>& members() const { return members_; }
    bool inFamily() const { return inFamily_; }
    std::size_t member() const { return member_; }
    static constexpr std::size_t kMostMembers = 8;
    // The family's picture, with each member's middle across it (in its pixels, in members' order),
    // for clicking them.
    void setFamilyPicture(const ts2::tga::Pixels& picture, std::vector<int> middles);

private:
    struct Tile {
        int button = -1, picture = -1;
        std::size_t choice = 0;
    };
    void showStep(Step step);
    void showSim();
    void showFamily();
    void showSimControls(const Sim& sim, std::optional<std::size_t> editing);
    // A box with the game's text `message` (CAS text table 0x8C) under title `title`, with OK.
    void tell(std::uint32_t title, std::uint32_t message);
    std::string casText(std::uint32_t value) const;
    void showPersonality();
    void setTrait(std::size_t trait, int points);
    void layoutChoices();
    int find(std::uint32_t id) const;

    ts2::ui::Screen& screen_;
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
    // Step 6: the trait bars (as the Sim's traits) and the points left.
    std::array<MeterBar, 5> traitBars_;
    MeterBar pointsLeft_;
    // The family: its members, its view shown or not, the member edited (none: a new Sim), the member
    // the last action was about, the members' middles across the preview, and the next new Sim's draw.
    std::vector<Sim> members_;
    bool inFamily_ = false;
    std::optional<std::size_t> editing_;
    std::size_t member_ = 0;
    std::vector<int> middles_;
    std::uint32_t nextSeed_ = 1;
    // The box shown over the screen, and what it asks.
    enum class Asking { Nothing, Told, CancelFamily, DeleteMember };
    DialogBox box_;
    Asking asking_ = Asking::Nothing;
};

} // namespace app
