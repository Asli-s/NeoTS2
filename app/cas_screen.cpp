#include "cas_screen.hpp"

#include "content/ui_meters.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <vector>

namespace app {

namespace {

constexpr std::uint32_t kCasScripts = 0x08000600;
constexpr std::uint32_t kCasMain = 0xCB980E50, kStepGenetics = 0xCB980E81, kStepHair = 0xCB980E82, kStepClothing = 0xCB980E84,
                        kStepPersonality = 0xCB980E83;
// The screen is laid out for 800 x 600.
constexpr int kLayoutWidth = 800, kLayoutHeight = 600;
// The main screen: the puck's Accept Family, Cancel Family, Create Family (a new Sim), Family
// Relationships, options, zoom and rotate buttons; the wizard's step buttons (step 1 to 6), DONE,
// Cancel Sim, Undo and Redo; the Sim's name under it; the household's name and biography.
constexpr std::uint32_t kAcceptFamily = 0x00000122, kCancelFamily = 0x00000105, kCreateFamily = 0xCCB70000, kRelationships = 0xCCB70001,
                        kCasOptions = 0xCCB70002, kZoomIn = 0x2D486AC2, kZoomOut = 0x2D486ACD, kRotateClockwise = 0x2CAFB772,
                        kRotateCounter = 0x4CAFB7AA;
constexpr std::uint32_t kFirstStep = 0x8C6B0000, kSteps = 6, kDone = 0xEC6B2109, kCancelSim = 0xED012A1C, kUndo = 0x8D24D87A,
                        kRedo = 0x8D24D88D;
constexpr std::uint32_t kSimName = 0x2CB7B3D6, kHousehold = 0x6C6AE0BD, kHouseholdName = 0x00000001;
constexpr std::uint32_t kRotationWindow = 0x4D097B6D;
// While a Sim is made, its controls (the wizard, Cancel Sim, Undo, Redo, its name) show in place of
// the household's bar.
constexpr std::uint32_t kCreateSimControls = 0x8C6B1CA2, kHouseholdBar = 0x0C327D9F;
// A step panel is placed by its marker (0x0000AAAA, captioned "2d050914:bl"): the marker's bottom
// left at the sub panel anchor, 471, 594 on the main screen.
constexpr std::uint32_t kPanelMarker = 0x0000AAAA;
constexpr int kAnchorX = 471, kAnchorY = 594;

// Step 1: the first name (inside its group), the biography (inside the background), Randomize, the
// Sim bin, Sim Bio, gender, age (toddler to elder), skin tones (1 to 4), custom skins and fitness.
constexpr std::uint32_t kFirstNameGroup = 0x8C6B1EB9, kTextBox = 0x00000001, kStepBackground = 0xAD06600A, kRandomize = 0xAC3405DF,
                        kSimBin = 0x8D014F8D, kSimBio = 0x8CB7C11F;
constexpr std::uint32_t kMale = 0x00000400, kFemale = 0x00000401;
constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 5> kAges = {
    {{0x00000200, 0x01}, {0x00000201, 0x02}, {0x00000202, 0x04}, {0x00000203, 0x08}, {0x00000204, 0x10}}};
constexpr std::uint32_t kFirstSkin = 0x2CB70000, kCustomSkins = 0x2CB70004, kCustomSkinsHighlight = 0x2D1259E3, kFitness = 0x2C720000,
                        kFitness2 = 0x2C720001;

// Step 3: its tabs (hair, then the face's parts), the hair component's grid, Previous, Next and
// Delete, the hair colours (black, brown, blonde, red), Custom, the outfit buttons and the per
// outfit / for all switch.
constexpr std::uint32_t kHairTab = 0x4CB80000, kFaceTabs = 0x4CB80001, kFaceTabCount = 6;
constexpr std::uint32_t kHairGrid = 0x8CB8C2B9, kHairPrevious = 0x8CB8C2C3, kHairNext = 0xACB8C2CF, kDeleteChoice = 0x2CFD6FDC;
constexpr std::uint32_t kFirstHairColour = 0x00008800, kCustomHairColour = 0x00008804;
constexpr std::array<std::uint32_t, 9> kHairOutfitButtons = {0x921BFDA1, 0x921BFDA2, 0x921BFDA3, 0x921BFDA4, 0x921BFDA5,
                                                             0x921BFDA6, 0x921BFDB0, 0x921BFDB1, 0x921BFDB2};

// Step 5: the outfit categories, the kinds of clothes, the grid, Previous and Next, and the
// collections' buttons.
constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 7> kClothingCategories = {
    {{0x02000000, 0x01}, {0x02000005, 0x08}, {0x02000006, 0x10}, {0x02000004, 0x04}, {0x02000003, 0x02}, {0x02000008, 0x40}, {0x02000009, 0x1000}}};
constexpr std::array<std::pair<std::uint32_t, CasScreen::Clothes>, 3> kClothingKinds = {
    {{0x01000000, CasScreen::Clothes::Tops}, {0x01000001, CasScreen::Clothes::Bottoms}, {0x01000002, CasScreen::Clothes::Outfits}}};
constexpr std::uint32_t kClothingGrid = 0x0BD73DA0, kClothingPrevious = 0x00000121, kClothingNext = 0x00000120, kClothingScroll = 0xEC8D06FF;
constexpr std::array<std::uint32_t, 6> kCollectionButtons = {0x0000F001, 0x0000F002, 0xECFD6F92, 0xCDEBBAEC, 0x6CFD0001, 0x2CFD6FDC};
constexpr std::uint32_t kAllCollections = 0x6CFD0000;

// Step 6: its tabs (aspiration and personality; turn-ons and offs), their windows, the zodiac's
// backgrounds and buttons (Aries first) and the sign's name, the aspirations of toddlers and
// children (Grow Up) and of the older ages, each aspiration's button (0x0CDC0000 + aspiration), the
// trait bars (neat, outgoing, active, playful, nice) with their fill, and the points left.
constexpr std::uint32_t kAspirationTab = 0x6FA06348, kTurnOnsTab = 0x6FA0634E, kTurnOnsWindow = 0x2FA06311,
                        kPersonalityWindow = 0x2FA06310;
constexpr std::uint32_t kZodiacBackground = 0x4CDC0000, kZodiacButton = 0xACDC0000, kZodiacName = 0x2BE35D53;
constexpr std::uint32_t kChildAspirations = 0x4DA3D18B, kOlderAspirations = 0x2DA3D170, kAspirationButton = 0x0CDC0000;
constexpr std::array<int, 6> kOlderAspirationKinds = {8, 2, 1, 6, 3, 5};
constexpr int kGrowUp = 7;
constexpr std::array<std::uint32_t, 5> kTraitBars = {0x4BDEFF03, 0x4BDEFF00, 0x4BDEFF04, 0x4BDEFF02, 0x4BDEFF01};
constexpr std::uint32_t kPointsLeft = 0x4BDEFF05, kBarFill = 0x00000003;
constexpr std::uint32_t kTraitPicture = 0xCA100130, kPointsLeftPicture = 0xCA100110;
// A trait bar's points start 6 pixels in, 9 pixels each.
constexpr int kTraitBarLeft = 6, kTraitPointWidth = 9, kTraitMax = 10;
constexpr std::array<const char*, 12> kZodiacNames = {"Aries", "Taurus", "Gemini",  "Cancer",    "Leo",      "Virgo",
                                                      "Libra", "Scorpio", "Sagittarius", "Capricorn", "Aquarius", "Pisces"};

// A grid shows two columns of three.
constexpr int kGridColumns = 2, kGridRows = 3;
constexpr std::size_t kPerPage = kGridColumns * kGridRows;
constexpr float kTurnStep = 45;
// A choice not worn is drawn dimmer.
constexpr std::array<float, 4> kWorn{1, 1, 1, 1}, kNotWorn{0.7f, 0.7f, 0.7f, 1};

ts2::ui::Window window(const std::string& kind, int w, int h, std::vector<std::pair<std::string, std::string>> more = {}) {
    ts2::ui::Window out;
    out.attributes = {{"iid", kind}, {"area", "(0,0," + std::to_string(w) + "," + std::to_string(h) + ")"}, {"winflag_visible", "yes"}};
    out.attributes.insert(out.attributes.end(), more.begin(), more.end());
    return out;
}

// Of the windows directly in `window` sharing an id (a version per product, Sims' and pets'), only
// the first is kept shown; the same down the tree.
void keepFirstVersions(ts2::ui::Screen& screen, int window) {
    std::map<std::uint32_t, int> seen;
    for (const int c : screen.children(window)) {
        const std::uint32_t id = screen.id(c);
        if (id != 0 && !seen.emplace(id, c).second) {
            screen.setVisible(c, false);
            continue;
        }
        keepFirstVersions(screen, c);
    }
}

// The backdrop behind Create-a-Sim: a soft blue, lighter towards the top.
ts2::tga::Pixels backdropPicture() {
    constexpr std::uint32_t kSide = 64;
    ts2::tga::Pixels out{kSide, kSide, std::vector<std::uint8_t>(kSide * kSide * 4)};
    for (std::uint32_t y = 0; y < kSide; ++y) {
        const float t = static_cast<float>(y) / (kSide - 1);
        for (std::uint32_t x = 0; x < kSide; ++x) {
            std::uint8_t* p = &out.rgba[(y * kSide + x) * 4];
            p[0] = static_cast<std::uint8_t>(150 - 70 * t);
            p[1] = static_cast<std::uint8_t>(180 - 70 * t);
            p[2] = static_cast<std::uint8_t>(215 - 60 * t);
            p[3] = 255;
        }
    }
    return out;
}

// A zodiac sign's traits in the bars' order: neat, outgoing, active, playful, nice.
std::array<int, 5> signTraits(int sign) {
    const auto t = ts2::content::zodiacTraits(sign);  // outgoing, nice, playful, neat, active
    return {t[3], t[0], t[4], t[2], t[1]};
}

// The sign the bars' traits point to.
int traitsSign(const std::array<int, 5>& traits) {
    return ts2::content::nearestZodiacSign(traits[1] * 100, traits[4] * 100, traits[3] * 100, traits[0] * 100, traits[2] * 100);
}

bool young(std::uint32_t age) { return age == 0x01 || age == 0x02; }

} // namespace

int CasScreen::find(std::uint32_t id) const {
    const int inMain = screen_.find(root_, id);
    return inMain >= 0 ? inMain : screen_.find(panel_, id);
}

void CasScreen::open(int width, int height, std::uint32_t seed) {
    close();
    sim_ = {};
    sim_.seed = seed;
    sim_.gender = seed % 2 == 0 ? 1 : 2;
    sim_.skin = static_cast<int>(seed / 2 % 4) + 1;
    sim_.hairColour = static_cast<int>(seed / 8 % 4) + 1;
    sim_.zodiac = static_cast<int>(seed / 32 % 12) + 1;
    sim_.traits = signTraits(sim_.zodiac);
    turn_ = 0;
    const int x = std::max(0, (width - kLayoutWidth) / 2), y = std::max(0, (height - kLayoutHeight) / 2);
    // The backdrop over everything else, the Sim's picture on it, and the screen over both.
    backdrop_ = screen_.addWindows({window("IGZWinBMP", width, height)}, 0, 0);
    screen_.setImage(backdrop_, backdropPicture());
    root_ = screen_.load(kCasScripts, kCasMain, x, y);
    if (root_ < 0) {
        close();
        return;
    }
    keepFirstVersions(screen_, root_);
    screen_.setVisible(screen_.find(root_, kCreateSimControls), true);
    screen_.setVisible(screen_.find(root_, kHouseholdBar), false);
    const int rotation = screen_.find(root_, kRotationWindow);
    const int w = rotation >= 0 ? screen_.width(rotation) : 440, h = rotation >= 0 ? screen_.height(rotation) : 546;
    preview_ = screen_.addWindows({window("IGZWinBMP", w, h)}, x + (rotation >= 0 ? screen_.x(rotation) : 0), y, backdrop_);
    // What isn't built yet is greyed.
    for (std::uint32_t id : {kAcceptFamily, kCreateFamily, kRelationships, kCasOptions, kZoomIn, kZoomOut, kDone, kCancelSim, kUndo, kRedo}) {
        screen_.setEnabled(find(id), false);
    }
    for (std::uint32_t i = 1; i < kSteps; ++i) {
        // Steps 3, 5 and 6 (hair, clothing, personality) are built.
        screen_.setEnabled(find(kFirstStep + i), i == 2 || i == 4 || i == 5);
    }
    family_ = TextField(screen_.find(screen_.find(root_, kHousehold), kHouseholdName), 64, false);
    family_.setText(screen_, "");
    showStep(Step::Genetics);
}

void CasScreen::close() {
    for (const int w : {root_, backdrop_}) {
        if (w >= 0) {
            screen_.remove(w);
        }
    }
    backdrop_ = root_ = panel_ = preview_ = grid_ = -1;
    first_ = family_ = bio_ = {};
    tiles_.clear();
    pictures_.clear();
    choices_ = 0;
}

void CasScreen::showStep(Step step) {
    step_ = step;
    if (panel_ >= 0) {
        screen_.remove(panel_);
    }
    tiles_.clear();
    grid_ = -1;
    first_ = bio_ = {};
    traitBars_ = {};
    pointsLeft_ = {};
    const std::uint32_t script = step == Step::Genetics ? kStepGenetics
                                 : step == Step::Hair   ? kStepHair
                                 : step == Step::Clothing ? kStepClothing
                                                          : kStepPersonality;
    panel_ = screen_.load(kCasScripts, script, 0, 0, root_);
    if (panel_ < 0) {
        return;
    }
    keepFirstVersions(screen_, panel_);
    // Placed by its marker, as the game places it.
    const int marker = screen_.find(panel_, kPanelMarker);
    const int mx = marker >= 0 ? screen_.x(marker) : 15, my = marker >= 0 ? screen_.y(marker) + screen_.height(marker) : 309;
    screen_.move(panel_, kAnchorX - mx, kAnchorY - my);
    for (std::uint32_t i = 0; i < kSteps; ++i) {
        const bool shown = (i == 0 && step == Step::Genetics) || (i == 2 && step == Step::Hair) || (i == 4 && step == Step::Clothing) ||
                           (i == 5 && step == Step::Personality);
        screen_.setChecked(find(kFirstStep + i), shown);
    }
    if (step == Step::Genetics) {
        for (std::uint32_t id : {kSimBin, kSimBio, kCustomSkins, kCustomSkinsHighlight, kFitness, kFitness2}) {
            screen_.setEnabled(find(id), false);
        }
        first_ = TextField(screen_.find(screen_.find(panel_, kFirstNameGroup), kTextBox), 64, false);
        bio_ = TextField(screen_.find(screen_.find(panel_, kStepBackground), kTextBox), 1024, true);
        first_.setText(screen_, sim_.firstName);
        bio_.setText(screen_, sim_.bio);
    } else if (step == Step::Hair) {
        // Only the hair tab, its colours and styles, set for all outfits.
        screen_.setChecked(find(kHairTab), true);
        for (std::uint32_t i = 0; i < kFaceTabCount; ++i) {
            screen_.setEnabled(find(kFaceTabs + i), false);
        }
        for (const std::uint32_t id : kHairOutfitButtons) {
            screen_.setEnabled(find(id), false);
        }
        for (std::uint32_t id : {kCustomHairColour, kDeleteChoice}) {
            screen_.setEnabled(find(id), false);
        }
        grid_ = find(kHairGrid);
    } else if (step == Step::Personality) {
        // The aspiration and personality tab; turn-ons and offs aren't built.
        screen_.setVisible(find(kTurnOnsWindow), false);
        screen_.setVisible(find(kPersonalityWindow), true);
        screen_.setChecked(find(kAspirationTab), true);
        screen_.setEnabled(find(kTurnOnsTab), false);
        screen_.setVisible(find(kChildAspirations), young(sim_.age));
        screen_.setVisible(find(kOlderAspirations), !young(sim_.age));
        for (std::size_t i = 0; i < kTraitBars.size(); ++i) {
            const int bar = find(kTraitBars[i]);
            traitBars_[i] = MeterBar(screen_, bar >= 0 ? screen_.find(bar, kBarFill) : -1, kTraitPicture, false);
        }
        const int left = find(kPointsLeft);
        pointsLeft_ = MeterBar(screen_, left >= 0 ? screen_.find(left, kBarFill) : -1, kPointsLeftPicture, true);
        showPersonality();
    } else {
        for (const std::uint32_t id : kCollectionButtons) {
            screen_.setEnabled(find(id), false);
        }
        screen_.setChecked(find(kAllCollections), true);
        screen_.setVisible(find(kClothingScroll), false);
        grid_ = find(kClothingGrid);
    }
    if (grid_ >= 0) {
        choiceWidth_ = screen_.width(grid_) / kGridColumns - 8;
        choiceHeight_ = screen_.height(grid_) / kGridRows - 8;
    }
    showSim();
}

void CasScreen::showSim() {
    screen_.setChecked(find(kFemale), sim_.gender == 1);
    screen_.setChecked(find(kMale), sim_.gender == 2);
    for (const auto& [id, age] : kAges) {
        screen_.setChecked(find(id), sim_.age == age);
    }
    for (int i = 0; i < 4; ++i) {
        screen_.setChecked(find(kFirstSkin + static_cast<std::uint32_t>(i)), sim_.skin == i + 1);
        screen_.setChecked(find(kFirstHairColour + static_cast<std::uint32_t>(i)), sim_.hairColour == i + 1);
    }
    for (const auto& [id, category] : kClothingCategories) {
        screen_.setChecked(find(id), sim_.category == category);
    }
    for (const auto& [id, kind] : kClothingKinds) {
        screen_.setChecked(find(id), sim_.clothes == kind);
    }
    screen_.setText(find(kSimName), sim_.firstName);
}

void CasScreen::showPersonality() {
    if (step_ != Step::Personality) {
        return;
    }
    int spent = 0;
    for (std::size_t i = 0; i < traitBars_.size(); ++i) {
        traitBars_[i].show(sim_.traits[i] * 100, kTraitMax);
        spent += sim_.traits[i];
    }
    pointsLeft_.show((kPersonalityPoints - spent) * 1000 / kPersonalityPoints, kPersonalityPoints);
    for (std::uint32_t sign = 1; sign <= 12; ++sign) {
        screen_.setVisible(find(kZodiacBackground + sign), static_cast<int>(sign) == sim_.zodiac);
        screen_.setChecked(find(kZodiacButton + sign), static_cast<int>(sign) == sim_.zodiac);
    }
    screen_.setText(find(kZodiacName), sim_.zodiac >= 1 && sim_.zodiac <= 12 ? kZodiacNames[sim_.zodiac - 1] : "");
    const int aspiration = young(sim_.age) ? kGrowUp : sim_.aspiration;
    screen_.setChecked(find(kAspirationButton + kGrowUp), aspiration == kGrowUp);
    for (const int kind : kOlderAspirationKinds) {
        screen_.setChecked(find(kAspirationButton + static_cast<std::uint32_t>(kind)), aspiration == kind);
    }
}

void CasScreen::setTrait(std::size_t trait, int points) {
    int others = 0;
    for (std::size_t i = 0; i < sim_.traits.size(); ++i) {
        others += i == trait ? 0 : sim_.traits[i];
    }
    // No more than the points left.
    sim_.traits[trait] = std::clamp(points, 0, std::min(kTraitMax, kPersonalityPoints - others));
    sim_.zodiac = traitsSign(sim_.traits);
    showPersonality();
}

void CasScreen::setChoices(std::size_t count, std::optional<std::size_t> worn) {
    choices_ = count;
    worn_ = worn;
    pictures_.assign(count, {});
    // The page with the one worn, else the first.
    firstChoice_ = worn && *worn < count ? *worn / kPerPage * kPerPage : 0;
    layoutChoices();
}

void CasScreen::layoutChoices() {
    for (const auto& t : tiles_) {
        screen_.remove(t.button);
    }
    tiles_.clear();
    const bool hair = step_ == Step::Hair;
    screen_.setEnabled(find(hair ? kHairPrevious : kClothingPrevious), firstChoice_ > 0);
    screen_.setEnabled(find(hair ? kHairNext : kClothingNext), firstChoice_ + kPerPage < choices_);
    if (grid_ < 0) {
        return;
    }
    const int parent = screen_.parent(grid_);
    const int cellW = screen_.width(grid_) / kGridColumns, cellH = screen_.height(grid_) / kGridRows;
    for (std::size_t i = firstChoice_; i < choices_ && i < firstChoice_ + kPerPage; ++i) {
        const int at = static_cast<int>(i - firstChoice_);
        auto button = window("IGZWinBtn", choiceWidth_, choiceHeight_);
        button.children.push_back(window("IGZWinBMP", choiceWidth_, choiceHeight_, {{"winflag_ignoremouse", "yes"}}));
        Tile t;
        t.choice = i;
        t.button = screen_.addWindows({button}, screen_.x(grid_) + at % kGridColumns * cellW + (cellW - choiceWidth_) / 2,
                                      screen_.y(grid_) + at / kGridColumns * cellH + (cellH - choiceHeight_) / 2, parent);
        t.picture = screen_.children(t.button).empty() ? -1 : screen_.children(t.button).front();
        tiles_.push_back(t);
        if (i < pictures_.size() && pictures_[i].width > 0) {
            setChoicePicture(i, pictures_[i]);
        }
    }
}

std::vector<std::size_t> CasScreen::shownChoices() const {
    std::vector<std::size_t> out;
    for (const auto& t : tiles_) {
        if (t.choice < pictures_.size() && pictures_[t.choice].width == 0) {
            out.push_back(t.choice);
        }
    }
    return out;
}

void CasScreen::setChoicePicture(std::size_t choice, const ts2::tga::Pixels& picture) {
    if (choice >= pictures_.size()) {
        return;
    }
    if (&pictures_[choice] != &picture) {
        pictures_[choice] = picture;
    }
    for (const auto& t : tiles_) {
        if (t.choice == choice && t.picture >= 0) {
            screen_.setImage(t.picture, picture);
            screen_.setImageColor(t.picture, worn_ && *worn_ == choice ? kWorn : kNotWorn);
        }
    }
}

CasScreen::Action CasScreen::clicked(int window) {
    if (root_ < 0 || window < 0) {
        return Action::None;
    }
    auto is = [&](std::uint32_t wanted) { return window == find(wanted); };
    if (is(kCancelFamily)) {
        return Action::Cancel;
    }
    if (is(kRotateClockwise) || is(kRotateCounter)) {
        turn_ += is(kRotateClockwise) ? -kTurnStep : kTurnStep;
        return Action::Turned;
    }
    // The steps.
    if (is(kFirstStep) && step_ != Step::Genetics) {
        showStep(Step::Genetics);
        return Action::None;
    }
    if (is(kFirstStep + 2) && step_ != Step::Hair) {
        showStep(Step::Hair);
        return Action::ChoicesNeeded;
    }
    if (is(kFirstStep + 4) && step_ != Step::Clothing) {
        showStep(Step::Clothing);
        return Action::ChoicesNeeded;
    }
    if (is(kFirstStep + 5) && step_ != Step::Personality) {
        showStep(Step::Personality);
        return Action::None;
    }
    // Step 6: a sign sets its traits; an aspiration is chosen (toddlers and children grow up).
    if (step_ == Step::Personality) {
        for (std::uint32_t sign = 1; sign <= 12; ++sign) {
            if (is(kZodiacButton + sign)) {
                sim_.zodiac = static_cast<int>(sign);
                sim_.traits = signTraits(sim_.zodiac);
            }
        }
        for (const int kind : kOlderAspirationKinds) {
            if (is(kAspirationButton + static_cast<std::uint32_t>(kind)) && !young(sim_.age)) {
                sim_.aspiration = kind;
            }
        }
        showPersonality();
        return Action::None;
    }
    // A choice, or a page of them.
    for (const auto& t : tiles_) {
        if (window == t.button) {
            chosen_ = t.choice;
            worn_ = t.choice;
            for (const auto& other : tiles_) {
                if (other.picture >= 0) {
                    screen_.setImageColor(other.picture, other.choice == t.choice ? kWorn : kNotWorn);
                }
            }
            return Action::Chosen;
        }
    }
    if ((is(kHairPrevious) || is(kClothingPrevious)) && firstChoice_ > 0) {
        firstChoice_ -= std::min(firstChoice_, kPerPage);
        layoutChoices();
        return Action::None;
    }
    if ((is(kHairNext) || is(kClothingNext)) && firstChoice_ + kPerPage < choices_) {
        firstChoice_ += kPerPage;
        layoutChoices();
        return Action::None;
    }
    // Step 1.
    Action result = Action::None;
    if (is(kFemale) || is(kMale)) {
        const std::uint32_t gender = is(kFemale) ? 1 : 2;
        result = sim_.gender != gender ? Action::Remake : Action::None;
        sim_.gender = gender;
    } else if (is(kRandomize)) {
        ++sim_.seed;  // another Sim of the same age, gender and skin
        result = Action::Remake;
    }
    for (const auto& [button, age] : kAges) {
        if (is(button)) {
            result = sim_.age != age ? Action::Remake : Action::None;
            sim_.age = age;
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (is(kFirstSkin + static_cast<std::uint32_t>(i))) {
            result = sim_.skin != i + 1 ? Action::Remake : Action::None;
            sim_.skin = i + 1;
        }
        // Step 3's hair colours.
        if (is(kFirstHairColour + static_cast<std::uint32_t>(i))) {
            result = sim_.hairColour != i + 1 ? Action::Recolour : Action::None;
            sim_.hairColour = i + 1;
        }
    }
    // Step 5's categories and kinds of clothes.
    for (const auto& [button, category] : kClothingCategories) {
        if (is(button) && sim_.category != category) {
            sim_.category = category;
            result = Action::Category;
        }
    }
    for (const auto& [button, kind] : kClothingKinds) {
        if (is(button) && sim_.clothes != kind) {
            sim_.clothes = kind;
            result = Action::ChoicesNeeded;
        }
    }
    showSim();
    return result;
}

void CasScreen::pointerDown(int window, int x, int /*y*/) {
    if (root_ < 0) {
        return;
    }
    for (TextField* field : {&first_, &family_, &bio_}) {
        field->focus(screen_, field->window() >= 0 && field->holds(screen_, window));
    }
    // Step 6: down on a trait bar sets the trait to the point under the mouse (left of the first,
    // none), as far as the points left go.
    if (step_ != Step::Personality) {
        return;
    }
    for (int w = window; w >= 0; w = screen_.parent(w)) {
        for (std::size_t i = 0; i < kTraitBars.size(); ++i) {
            if (w == find(kTraitBars[i])) {
                int left = 0;
                for (int p = w; p >= 0; p = screen_.parent(p)) {
                    left += screen_.x(p);
                }
                const int into = x - left - kTraitBarLeft;
                setTrait(i, into <= 0 ? 0 : (into + kTraitPointWidth - 1) / kTraitPointWidth);
                return;
            }
        }
    }
}

bool CasScreen::editing() const { return first_.focused() || family_.focused() || bio_.focused(); }

void CasScreen::typed(const std::string& text) {
    for (TextField* field : {&first_, &family_, &bio_}) {
        field->typed(screen_, text);
    }
    if (step_ == Step::Genetics) {
        sim_.firstName = first_.text();
        sim_.bio = bio_.text();
    }
    showSim();
}

void CasScreen::key(ts2::platform::Key key) {
    for (TextField* field : {&first_, &family_, &bio_}) {
        field->key(screen_, key);
    }
    if (step_ == Step::Genetics) {
        sim_.firstName = first_.text();
        sim_.bio = bio_.text();
    }
    showSim();
}

void CasScreen::setPreview(const ts2::tga::Pixels& picture) {
    if (preview_ >= 0) {
        screen_.setImage(preview_, picture);
    }
}

int CasScreen::previewWidth() const { return preview_ >= 0 ? screen_.width(preview_) : 0; }
int CasScreen::previewHeight() const { return preview_ >= 0 ? screen_.height(preview_) : 0; }

} // namespace app
