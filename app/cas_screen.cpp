#include "cas_screen.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <vector>

namespace app {

namespace {

constexpr std::uint32_t kCasScripts = 0x08000600;
constexpr std::uint32_t kCasMain = 0xCB980E50, kStepGenetics = 0xCB980E81;
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
// Step 1: the first name (inside its group), the biography (inside the background), Randomize, the
// Sim bin, Sim Bio, gender, age (toddler to elder), skin tones (1 to 4), custom skins and fitness.
constexpr std::uint32_t kFirstNameGroup = 0x8C6B1EB9, kTextBox = 0x00000001, kStepBackground = 0xAD06600A, kRandomize = 0xAC3405DF,
                        kSimBin = 0x8D014F8D, kSimBio = 0x8CB7C11F;
constexpr std::uint32_t kMale = 0x00000400, kFemale = 0x00000401;
constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 5> kAges = {
    {{0x00000200, 0x01}, {0x00000201, 0x02}, {0x00000202, 0x04}, {0x00000203, 0x08}, {0x00000204, 0x10}}};
constexpr std::uint32_t kFirstSkin = 0x2CB70000, kCustomSkins = 0x2CB70004, kCustomSkinsHighlight = 0x2D1259E3, kFitness = 0x2C720000,
                        kFitness2 = 0x2C720001;
// Where step panels sit: their marker (0x0000AAAA, captioned "2d050914:bl") at the sub panel anchor
// (0x2D050914, at 471, 594), so a panel's top left is 456, 285.
constexpr int kStepX = 456, kStepY = 285;
// While a Sim is made, its controls (the wizard, Cancel Sim, Undo, Redo, its name) show in place of
// the household's bar.
constexpr std::uint32_t kCreateSimControls = 0x8C6B1CA2, kHouseholdBar = 0x0C327D9F;
constexpr float kTurnStep = 45;

ts2::ui::Window window(const std::string& kind, int w, int h) {
    ts2::ui::Window out;
    out.attributes = {{"iid", kind}, {"area", "(0,0," + std::to_string(w) + "," + std::to_string(h) + ")"}, {"winflag_visible", "yes"}};
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

} // namespace

int CasScreen::find(std::uint32_t id) const {
    const int inMain = screen_.find(root_, id);
    return inMain >= 0 ? inMain : screen_.find(step_, id);
}

void CasScreen::open(int width, int height, std::uint32_t seed) {
    close();
    sim_ = {};
    sim_.seed = seed;
    sim_.gender = seed % 2 == 0 ? 1 : 2;
    sim_.skin = static_cast<int>(seed / 2 % 4) + 1;
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
    step_ = screen_.load(kCasScripts, kStepGenetics, kStepX, kStepY, root_);
    if (step_ >= 0) {
        keepFirstVersions(screen_, step_);
    }
    // What isn't built yet is greyed.
    for (std::uint32_t id : {kAcceptFamily, kCreateFamily, kRelationships, kCasOptions, kZoomIn, kZoomOut, kDone, kCancelSim, kUndo, kRedo,
                             kSimBin, kSimBio, kCustomSkins, kCustomSkinsHighlight, kFitness, kFitness2}) {
        screen_.setEnabled(find(id), false);
    }
    for (std::uint32_t i = 1; i < kSteps; ++i) {
        screen_.setEnabled(find(kFirstStep + i), false);
    }
    screen_.setChecked(find(kFirstStep), true);
    first_ = TextField(screen_.find(screen_.find(step_, kFirstNameGroup), kTextBox), 64, false);
    bio_ = TextField(screen_.find(screen_.find(step_, kStepBackground), kTextBox), 1024, true);
    family_ = TextField(screen_.find(screen_.find(root_, kHousehold), kHouseholdName), 64, false);
    first_.setText(screen_, "");
    bio_.setText(screen_, "");
    family_.setText(screen_, "");
    showSim();
}

void CasScreen::close() {
    for (const int w : {root_, backdrop_}) {
        if (w >= 0) {
            screen_.remove(w);
        }
    }
    backdrop_ = root_ = step_ = preview_ = -1;
    first_ = family_ = bio_ = {};
}

void CasScreen::showSim() {
    screen_.setChecked(find(kFemale), sim_.gender == 1);
    screen_.setChecked(find(kMale), sim_.gender == 2);
    for (const auto& [id, age] : kAges) {
        screen_.setChecked(find(id), sim_.age == age);
    }
    for (int i = 0; i < 4; ++i) {
        screen_.setChecked(find(kFirstSkin + static_cast<std::uint32_t>(i)), sim_.skin == i + 1);
    }
    screen_.setText(find(kSimName), sim_.firstName.empty() ? std::string() : sim_.firstName);
}

CasScreen::Action CasScreen::clicked(int window) {
    if (root_ < 0 || window < 0) {
        return Action::None;
    }
    const std::uint32_t id = screen_.id(window);
    auto is = [&](std::uint32_t wanted) { return window == find(wanted); };
    if (is(kCancelFamily)) {
        return Action::Cancel;
    }
    if (is(kRotateClockwise) || is(kRotateCounter)) {
        turn_ += is(kRotateClockwise) ? -kTurnStep : kTurnStep;
        return Action::Turned;
    }
    bool changed = false;
    if (is(kFemale) || is(kMale)) {
        changed = sim_.gender != (is(kFemale) ? 1u : 2u);
        sim_.gender = is(kFemale) ? 1 : 2;
    } else if (is(kRandomize)) {
        ++sim_.seed;  // another Sim of the same age, gender and skin
        changed = true;
    } else {
        for (const auto& [button, age] : kAges) {
            if (is(button)) {
                changed = sim_.age != age;
                sim_.age = age;
            }
        }
        for (int i = 0; i < 4; ++i) {
            if (id == kFirstSkin + static_cast<std::uint32_t>(i) && is(id)) {
                changed = sim_.skin != i + 1;
                sim_.skin = i + 1;
            }
        }
    }
    showSim();
    return changed ? Action::LookChanged : Action::None;
}

void CasScreen::pointerDown(int window) {
    if (root_ < 0) {
        return;
    }
    for (TextField* field : {&first_, &family_, &bio_}) {
        field->focus(screen_, field->window() >= 0 && field->holds(screen_, window));
    }
}

bool CasScreen::editing() const { return first_.focused() || family_.focused() || bio_.focused(); }

void CasScreen::typed(const std::string& text) {
    for (TextField* field : {&first_, &family_, &bio_}) {
        field->typed(screen_, text);
    }
    sim_.firstName = first_.text();
    sim_.bio = bio_.text();
    showSim();
}

void CasScreen::key(ts2::platform::Key key) {
    for (TextField* field : {&first_, &family_, &bio_}) {
        field->key(screen_, key);
    }
    sim_.firstName = first_.text();
    sim_.bio = bio_.text();
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
