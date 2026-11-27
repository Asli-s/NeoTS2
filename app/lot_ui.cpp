#include "lot_ui.hpp"

#include "content/build_scripts.hpp"
#include "content/ui_meters.hpp"
#include "content/ui_queue.hpp"
#include "content/ui_text.hpp"
#include "content/wants.hpp"
#include "diag/timing.hpp"
#include "diagnostics.hpp"
#include "formats/hash.hpp"
#include "names.hpp"
#include "sim/want_refill.hpp"
#include "sim/selection.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <functional>
#include <string>

namespace app {

namespace {

bool sameWants(const std::vector<ts2::swaf::Want>& a, const std::vector<ts2::swaf::Want>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const ts2::swaf::Want& x, const ts2::swaf::Want& y) {
        return x.version == y.version && x.id == y.id && x.guid == y.guid && x.type == y.type && x.argument == y.argument &&
               x.value == y.value && x.tail == y.tail && x.flags == y.flags;
    });
}

constexpr std::uint32_t kScripts = 0xA99D8A11;
constexpr std::uint32_t kPuck = 0x4905F85B, kLivePanel = 0x49060005, kOptionsMenu = 0x49060001;
// The live panel for a selected pet.
constexpr std::uint32_t kPetPanel = 0xFEED2000;
// The puck's windows.
constexpr std::uint32_t kLiveMode = 0x37D97B25, kBuyMode = 0x67D97B2A, kBuildMode = 0x77D97B28, kOptions = 0x67D97B2F, kFunds = 0x00003000;
constexpr std::uint32_t kBuyPanel = 0x49060004, kBuildPanel = 0x49060003, kHeldPanel = 0x49060007;
constexpr std::uint32_t kFloorUp = 0x00002001, kFloorDown = 0x00002000, kToggleCeiling = 0x00007000;
// The camera buttons: rotate left and right, zoom in and out.
constexpr std::uint32_t kCameraButtons[] = {0x00001001, 0x00001000, 0x00001002, 0x00001003};
// One wall button per wall mode, in the same place. The one for the mode in use shows.
constexpr std::uint32_t kWallButtons = 0x00001100;
constexpr int kWallModes = 4;
// The wall menu: buttons 1 to 4 pick walls down, cutaway, walls up and roofs. It closes once the
// mouse leaves its hidden area window.
constexpr std::uint32_t kWallMenu = 0x00004000, kWallMenuArea = 0x00000005;
// Advanced Neighborhood Options: a panel at (0, 40) with a button that opens and closes a strip of
// neighborhood buttons ending in the neighborhood's name. The name takes the first of its three
// texts (large, medium, small) no wider than 200, the small one when none is, at least 50 wide, and
// the strip's background ends 20 past it. Each chooser keeps the versions its script shows, so the
// unattached one (the plus) is drawn over the attached one.
constexpr std::uint32_t kHoodOptions = 0x0BB40010, kHoodStrip = 0x46BE0001, kHoodButtonHolder = 0x46BE0000, kHoodButton = 0x46000005;
constexpr std::uint32_t kHoodBackground = 0x00000001, kHoodReturn = 0x00000005, kHoodMainMenu = 0x00000002;
// The open and close button's tip: Advanced Neighborhood Options while the strip is closed, Minimize
// while it is open (UI Text 0xBB41).
constexpr std::uint32_t kUiText = 0x7FA7DC7C, kHoodOpenTip = 0xBB41004B, kHoodCloseTip = 0xBB41004C;  // string ids, leakcheck: ok
constexpr std::uint32_t kHoodNames[] = {0x00000003, 0x000000b3, 0x000000a3};
constexpr int kHoodPanelY = 40, kHoodNameWidest = 200, kHoodNameNarrowest = 50, kHoodNameToEnd = 20;
// The puck script's second top window: the version text, hidden until set up (see setUpVersion).
constexpr std::uint32_t kVersionText = 0x12345678;
// It sits this far in from the screen's top right corner.
constexpr int kVersionInset = 5;
// The live panel's windows.
constexpr std::uint32_t kClock = 0x37D82A93, kPause = 0x27D7258F, kNormal = 0x57E01CC1, kHigh = 0x37E01CC3,
                        kUltra = 0x47E01CC5;
constexpr std::uint32_t kNoPetsBackground = 0xA94E97E4, kPetsBackground = 0xA94E97E3;
// The pet panel's season indicator, which isn't inside its frame as the Sims' panel's is.
constexpr std::uint32_t kPetSeasons = 0x7235F4C7;
constexpr std::uint32_t kPortraitHolder = 0x4BDEFFA4, kVacationDays = 0x1320BD2F;
// In the portrait holder: the selected Sim's whole portrait, its shadow, and a small picture whose use isn't known.
// A saved want's argument kind for a Sim: its argument is the Sim's neighbor id.
// Want icons and the memory popup's portrait are this big.
constexpr std::uint32_t kWantIconSize = 44, kWantPopupPictureSize = 46, kMemoryPortraitSize = 64;
// A held want (the player's right click) keeps bit 0x01 of its flags.
constexpr std::uint8_t kHeldWant = 0x01;
// Person data: the lifetime aspiration's happiness (0..30000), shown on the bar behind the aspiration bar.
constexpr std::size_t kLifetimeHappiness = 0xD8;
// The pause button's blink: pressed and plain by turns, each this long.
constexpr auto kPauseBlink = std::chrono::milliseconds(500);
// Person data: the influence level (0..4, which influence bar picture shows) and influence points.
constexpr std::size_t kInfluenceLevel = 0xB0, kInfluencePoints = 0xB2;
// Person data for the age tooltip: the stage's length and days left, the days lived before
// becoming an elder, and the flags whose bit 0x100 marks a PlantSim.
constexpr std::size_t kAgeDuration = 0x9C, kAgeDaysLeft = 0x5B, kDaysBeforeElder = 0x5C, kPersonFlags = 0x54;
constexpr std::int16_t kPlantSim = 0x100;
// For the rewards panel: the Sim's age and whether it is a young adult, and the token that changes
// which aspiration's button shows.
constexpr std::size_t kAge = 0x3A, kYoungAdult = 0xAF;
constexpr std::uint32_t kAspirationToken = 0x53D08989;
// The talent badges' tokens. A badge counts once its token's value 0 reaches bronze (333). The game
// reads the value unsigned, so a negative one counts as gold.
constexpr std::uint32_t kBadgeTokens[] = {0x505FDC28, 0x705FDC8F, 0x905FDBB5, 0x505FDBC6, 0xB05FDC1B, 0x105FDC52,
                                          0xD05FDB91, 0x1215A8C1, 0x12297157, 0xF3D9B8DA, 0xD3DB2A27};
constexpr std::int16_t kBronzeBadge = 333;
// The token whose values list the hobbies (by person data index) whose Secret Lot the Sim has found.
constexpr std::uint32_t kSecretLotToken = 0x53F9DF4E;
// The sizes of the rewards panel's pictures: inventory and aspiration rewards, career rewards.
constexpr std::uint32_t kSmallRewardPicture = 60, kLargeRewardWidth = 80, kLargeRewardHeight = 72;
// The career icon on career rewards (window 0xA006).
constexpr std::uint32_t kCareerIconSide = 24;
// The reward objects: OBJD field 0x28's bits, the points (person data 0xA1, in tens), Free Time's
// pack bit and the lifetime aspiration levels earned and spent.
constexpr std::size_t kRewardKind = 0x28;
constexpr std::uint16_t kAspirationReward = 0x400, kCareerReward = 0x800;
constexpr std::size_t kRewardPoints = 0xA1, kLifetimeLevels = 0xD9, kLifetimeLevelsSpent = 0xDA;
constexpr std::uint32_t kFreeTime = 0x2000;
constexpr std::uint32_t kPanelPortrait = 0x00000001, kPanelShadow = 0x00000002, kPortraitConstants = 0x00000004, kPanelStar = 0x4BDEFFAA;
// The options menu's windows.
constexpr std::uint32_t kNeighborhood = 0x0000A210, kExitTutorial = 0x0000A211, kQuit = 0x0000A220, kSave = 0x0000A230;

// A window's own child with an id (the puck's skewer thumbnails reuse some of the puck's ids).
int child(const ts2::ui::Screen& screen, int parent, std::uint32_t id) {
    if (parent < 0) {
        return -1;
    }
    for (const int c : screen.children(parent)) {
        if (screen.id(c) == id) {
            return c;
        }
    }
    return -1;
}

// Funds as the puck shows them: the Simoleon sign and thousands separators.
std::string fundsText(std::int32_t funds) {
    std::string digits = std::to_string(std::abs(std::int64_t{funds}));
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<std::size_t>(i), ",");
    }
    return std::string(funds < 0 ? "-" : "") + "\xC2\xA7" + digits;
}

// A relationship's chemistry level (1-7) as far as it is ported: none until the relationship is
// listed (flag bit 15), 4 for family that isn't married (flag bit 3). The rest (from both Sims'
// value 8, a tuning table and mutual attraction) is not done yet, so it is 0.
int chemistryLevel(const LotLife::Relationship& r) {
    if ((r.flags & 0x8000) == 0) {
        return 0;
    }
    return r.familyTie && (r.flags & 0x08) == 0 ? 4 : 0;
}

// The covering catalog's buttons show pictures this many pixels on a side.
constexpr std::uint32_t kCoveringPictureSide = 40;

// A picture in a window as the thumbnail service puts it: at its own size centred when it fits,
// else scaled to the window.
ts2::tga::Pixels fitted(const ts2::tga::Pixels& image, std::uint32_t width, std::uint32_t height) {
    if (image.width <= width && image.height <= height) {
        return ts2::content::placed(image, width, height, static_cast<int>(width - image.width) / 2,
                                    static_cast<int>(height - image.height) / 2);
    }
    return ts2::content::scaled(image, width, height);
}

// The live panel's portrait windows as its script describes them: the portrait's picture (the shape
// the selected Sim's portrait is cut to, with rounded corners and a curve at the bottom left by the
// puck), its shadow's picture and how far the shadow is moved from the portrait, in the portrait's
// pixels. The game moves it 5 and 5 when the script doesn't say.
PortraitWindows portraitWindows(ts2::ui::Screen& screen) {
    PortraitWindows out;
    const auto script = screen.data().script(kScripts, kLivePanel);
    if (!script) {
        return out;
    }
    const std::function<const ts2::ui::Window*(const std::vector<ts2::ui::Window>&, std::uint32_t)> find =
        [&](const std::vector<ts2::ui::Window>& windows, std::uint32_t id) -> const ts2::ui::Window* {
        for (const auto& w : windows) {
            if (w.number("id") == id) {
                return &w;
            }
            if (const auto* inner = find(w.children, id)) {
                return inner;
            }
        }
        return nullptr;
    };
    const auto* holder = find(*script, kPortraitHolder);
    if (holder == nullptr) {
        return out;
    }
    const auto picture = [&](std::uint32_t id) {
        const auto* window = find(holder->children, id);
        const auto image = window ? window->list("image") : std::vector<std::int64_t>{};
        if (image.size() < 2) {
            return ts2::tga::Pixels{};
        }
        return screen.data()
            .image(static_cast<std::uint32_t>(image[image.size() - 2]), static_cast<std::uint32_t>(image.back()))
            .value_or(ts2::tga::Pixels{});
    };
    out.shape = picture(kPanelPortrait);
    out.shadowShape = picture(kPanelShadow);
    if (const auto* constants = find(holder->children, kPortraitConstants)) {
        std::vector<std::string> lines;
        for (const auto& w : constants->children) {
            lines.push_back(w.text("caption"));
        }
        out.shadowX = ts2::content::uiConstant(lines, "kShadowOffsetX").value_or(out.shadowX);
        out.shadowY = ts2::content::uiConstant(lines, "kShadowOffsetY").value_or(out.shadowY);
    }
    return out;
}

} // namespace

LotUi::LotUi(ts2::ui::Screen& screen, int width, int height) : screen_(screen), width_(width), height_(height) {}

void LotUi::load() {
    if (loaded_ == 0) {
        puck_ = screen_.load(kScripts, kPuck, 0, 0);
        live_ = screen_.load(kScripts, kLivePanel, 0, 0);
        petLive_ = screen_.load(kScripts, kPetPanel, 0, 0);
        options_ = screen_.load(kScripts, kOptionsMenu, 0, 0);
        buy_ = screen_.load(kScripts, kBuyPanel, 0, 0);
        build_ = screen_.load(kScripts, kBuildPanel, 0, 0);
        held_ = screen_.load(kScripts, kHeldPanel, 0, 0);
        hoodPanel_ = screen_.load(kScripts, kHoodOptions, 0, kHoodPanelY);
        hoodStrip_ = screen_.find(hoodPanel_, kHoodStrip);
        hoodButtonHolder_ = screen_.find(hoodPanel_, kHoodButtonHolder);
        hoodReturnTip_ = screen_.tip(screen_.find(hoodStrip_, kHoodReturn));
        close();
        // Without all three there is nothing more to set up.
        loaded_ = puck_ < 0 || live_ < 0 || options_ < 0 ? 2 : 1;
    } else if (loaded_ == 1) {
        loaded_ = 2;
        // The puck sits in the bottom left corner and the panels hang off it.
        screen_.move(puck_, 0, height_ - screen_.height(puck_));
        anchor(live_);
        anchor(petLive_);
        anchor(options_);
        anchor(buy_);
        anchor(build_);
        anchor(held_);
        buyPanel_.attach(screen_, buy_);
        buildPanel_.attach(screen_, build_);
        screen_.raise(puck_);
        // The clock and speed buttons, and the frame the live panel lays out. The rest (portrait,
        // influence and aspiration meters) isn't built yet.
        const int clock = screen_.find(live_, kClock), pause = screen_.find(live_, kPause);
        const int speeds = screen_.parent(screen_.find(live_, kNormal));
        clock_ = clock;
        speedButtons_ = {pause, screen_.find(live_, kNormal), screen_.find(live_, kHigh), screen_.find(live_, kUltra)};
        for (const int part : screen_.children(live_)) {
            screen_.setVisible(part, part == clock || part == pause || part == speeds);
        }
        panel_.attach(screen_, live_);
        // The pets' panel the same way, its frame laid out by the pet panel.
        if (petLive_ >= 0) {
            petClock_ = screen_.find(petLive_, kClock);
            petSpeedButtons_ = {screen_.find(petLive_, kPause), screen_.find(petLive_, kNormal), screen_.find(petLive_, kHigh),
                                screen_.find(petLive_, kUltra)};
            const int petSpeeds = screen_.parent(petSpeedButtons_[1]);
            for (const int part : screen_.children(petLive_)) {
                screen_.setVisible(part, part == petSpeeds || screen_.id(part) == kPetSeasons);
            }
            petPanel_.attach(screen_, petLive_);
            screen_.setVisible(screen_.find(petLive_, kPortraitHolder), false);
        }
        portraitWindows_ = portraitWindows(screen_);
        skewer_.attach(screen_, puck_);
        pauseFrame_.attach(screen_, width_, height_);
        queue_.attach(screen_);
        screen_.setVisible(screen_.find(live_, kPortraitHolder), false);
        screen_.setVisible(screen_.find(live_, kVacationDays), false);
        // A household without pets shows the plain background.
        showPetsBackground(false);
        screen_.setChecked(screen_.find(puck_, kLiveMode), true);
        // Exit Tutorial shares the Neighborhood button's place. It only shows in the tutorial.
        screen_.setVisible(screen_.find(options_, kExitTutorial), false);
        funds_ = child(screen_, puck_, kFunds);
        for (std::size_t i = 0; i < cameraButtons_.size(); ++i) {
            cameraButtons_[i] = child(screen_, puck_, kCameraButtons[i]);
        }
        wallMenu_ = child(screen_, puck_, kWallMenu);
        screen_.setVisible(wallMenu_, false);
        optionsMenu_.setMenu(options_);
        gameOptions_.load(GameOptionsPanel::Place::Lot);
        graphicsOptions_.load(GraphicsOptionsPanel::Place::Lot);
        audioOptions_.load();
        setUpVersion();
    }
}

void LotUi::setSize(int width, int height) {
    const int down = height - height_;
    width_ = width;
    height_ = height;
    if (loaded_ < 2) {
        return;
    }
    for (const int panel : {puck_, live_, petLive_, options_, buy_, build_, held_}) {
        if (panel >= 0) {
            screen_.move(panel, screen_.x(panel), screen_.y(panel) + down);
        }
    }
    pauseFrame_.layout(width, height);
    if (version_ >= 0) {
        screen_.move(version_, width_ - screen_.width(version_) - kVersionInset, kVersionInset);
    }
}

void LotUi::setUpVersion() {
    // The game shows its version here only with testingCheatsEnabled on as the puck is set up. Turning
    // it on later doesn't show it. Any build but a release shows it always.
    if (releaseBuild() && !testingCheats_) {
        return;
    }
    version_ = screen_.findTop(kVersionText);
    if (version_ < 0) {
        return;
    }
    // A release shows only its number, as each number is released from one commit. Other builds show
    // the commit too.
    std::string version = buildVersion();
    if (releaseBuild()) {
        version = version.substr(0, version.find('+'));
    }
    screen_.setText(version_, "v" + version);
    screen_.fitToText(version_);
    screen_.move(version_, width_ - screen_.width(version_) - kVersionInset, kVersionInset);
}

void LotUi::open() {
    while (loaded_ < 2) {
        load();
    }
    for (const int panel : {puck_, live_, version_}) {
        if (panel >= 0) {
            screen_.setVisible(panel, true);
        }
    }
    // The last lot's household goes. This one's comes with its simulation, and until then Live
    // can't be used.
    hasMembers_ = false;
    skewer_.setHousehold({});
    queue_.show({});
    showOptions(false);
}

void LotUi::close() {
    notifications_.clear(screen_);
    closeWallMenu();
    pauseFrame_.show(false);
    queue_.setVisible(false);
    for (const int panel : {puck_, live_, petLive_, options_, buy_, build_, held_, version_}) {
        if (panel >= 0) {
            screen_.setVisible(panel, false);
        }
    }
    showHoodPanel(false);
}

void LotUi::setNeighborhood(const std::string& name) {
    if (hoodStrip_ < 0) {
        return;
    }
    // Return to %s names the neighborhood.
    if (std::string tip = hoodReturnTip_; tip.find("%s") != std::string::npos) {
        screen_.setTip(screen_.find(hoodStrip_, kHoodReturn), tip.replace(tip.find("%s"), 2, name));
    }
    int picked = -1, smallest = -1;
    for (const std::uint32_t id : kHoodNames) {
        const int text = screen_.find(hoodStrip_, id);
        if (text < 0) {
            continue;
        }
        smallest = text;
        screen_.setText(text, name);
        screen_.fitToText(text);
        if (picked < 0 && screen_.width(text) <= kHoodNameWidest) {
            picked = text;
        }
    }
    if (picked < 0) {
        picked = smallest;
    }
    for (const std::uint32_t id : kHoodNames) {
        const int text = screen_.find(hoodStrip_, id);
        screen_.setVisible(text, text == picked);
    }
    if (picked < 0) {
        return;
    }
    screen_.resize(picked, std::max(screen_.width(picked), kHoodNameNarrowest), screen_.height(picked));
    const int background = screen_.find(hoodStrip_, kHoodBackground);
    screen_.resize(background, screen_.x(picked) + screen_.width(picked) + kHoodNameToEnd - screen_.x(background), screen_.height(background));
}

// The panel shows while the options menu and the puck do, and it always opens with its strip closed.
void LotUi::showHoodPanel(bool show) {
    if (hoodPanel_ < 0) {
        return;
    }
    screen_.setVisible(hoodPanel_, show);
    screen_.setVisible(hoodButtonHolder_, true);
    showHoodStrip(false);
}

void LotUi::showHoodStrip(bool show) {
    screen_.setVisible(hoodStrip_, show);
    screen_.setTip(screen_.find(hoodButtonHolder_, kHoodButton), screen_.data().text(kUiText, show ? kHoodCloseTip : kHoodOpenTip));
}

void LotUi::hideForFreeCamera(bool hide) {
    if (hide == hidden_) {
        return;
    }
    hidden_ = hide;
    skewer_.setHidden(hide);
    if (hide) {
        modeBeforeHide_ = mode_;
        closeWallMenu();
        queue_.setVisible(false);
        for (const int panel : {puck_, live_, petLive_, options_, buy_, build_, held_, version_}) {
            if (panel >= 0) {
                screen_.setVisible(panel, false);
            }
        }
        showHoodPanel(false);
        return;
    }
    for (const int panel : {puck_, version_}) {
        if (panel >= 0) {
            screen_.setVisible(panel, true);
        }
    }
    showLivePanel(true);
    setMode(modeBeforeHide_);
}

int LotUi::left(int window) const {
    int x = 0;
    for (int w = window; w >= 0; w = screen_.parent(w)) {
        x += screen_.x(w);
    }
    return x;
}

int LotUi::top(int window) const {
    int y = 0;
    for (int w = window; w >= 0; w = screen_.parent(w)) {
        y += screen_.y(w);
    }
    return y;
}

void LotUi::anchor(int panel) {
    // The anchor is a direct or nested child. Its caption names the puck window it belongs on.
    auto find = [&](auto&& self, int window) -> int {
        for (const int c : screen_.children(window)) {
            const std::string& caption = screen_.text(c);
            if (caption.size() > 3 && caption.ends_with(":bl")) {
                return c;
            }
            if (const int found = self(self, c); found >= 0) {
                return found;
            }
        }
        return -1;
    };
    const int mark = find(find, panel);
    if (mark < 0) {
        return;
    }
    const std::string& caption = screen_.text(mark);
    const auto target = screen_.find(puck_, static_cast<std::uint32_t>(std::stoul(caption.substr(0, caption.size() - 3), nullptr, 16)));
    if (target < 0) {
        return;
    }
    screen_.move(panel, left(target) - (left(mark) - screen_.x(panel)), top(target) - (top(mark) - screen_.y(panel)));
}

LotUi::Action LotUi::modeKey(Mode mode) {
    // The Design dialog holds the mouse, and the mode keys with it.
    if (designDialog_.isOpen()) {
        return Action::None;
    }
    const std::uint32_t id = mode == Mode::Live ? kLiveMode : mode == Mode::Buy ? kBuyMode : mode == Mode::Build ? kBuildMode : 0;
    const int button = id != 0 ? screen_.find(puck_, id) : -1;
    return button >= 0 ? clicked(button) : Action::None;
}

void LotUi::setMode(Mode mode) {
    const bool live = mode == Mode::Live;
    mode_ = mode;
    // The queue goes with the live panel.
    queue_.setVisible(live);
    if (options_ < 0) {
        return;
    }
    screen_.setVisible(options_, mode == Mode::Options);
    if (mode != Mode::Options) {
        optionsMenu_.close();
    }
    screen_.setVisible(buy_, mode == Mode::Buy);
    if (mode != Mode::Buy) {
        buyPanel_.grid().closeDescription();
    }
    if (mode != Mode::Build) {
        buildPanel_.grid().closeDescription();
    }
    screen_.setVisible(build_, mode == Mode::Build);
    if (mode == Mode::Buy) {
        buyPanel_.open();
    } else if (mode == Mode::Build) {
        buildPanel_.open();
    }
    showHoodPanel(mode == Mode::Options && puck_ >= 0 && screen_.visible(puck_));
    // With nobody in the household, Live can't be used: its button is disabled and the live panel
    // stays hidden.
    showLivePanel(live && hasMembers_);
    skewer_.setLiveMode(live);
    screen_.setChecked(screen_.find(puck_, kOptions), mode == Mode::Options);
    screen_.setChecked(child(screen_, puck_, kBuyMode), mode == Mode::Buy);
    screen_.setChecked(child(screen_, puck_, kBuildMode), mode == Mode::Build);
    const int liveButton = screen_.find(puck_, kLiveMode);
    screen_.setEnabled(liveButton, hasMembers_);
    screen_.setChecked(liveButton, live && hasMembers_);
}

// The Sims' live panel shows its background with the pets' branch while the skewer has one.
void LotUi::showPetsBackground(bool pets) {
    screen_.setVisible(screen_.find(live_, kPetsBackground), pets);
    screen_.setVisible(screen_.find(live_, kNoPetsBackground), !pets);
}

void LotUi::showLivePanel(bool show) {
    screen_.setVisible(live_, show && !petShown_);
    screen_.setVisible(petLive_, show && petShown_);
}

void LotUi::openWallMenu() {
    if (wallMenu_ < 0 || screen_.visible(wallMenu_)) {
        return;
    }
    for (int mode = 0; mode < kWallModes; ++mode) {
        screen_.setChecked(child(screen_, wallMenu_, static_cast<std::uint32_t>(mode + 1)), mode == wallMode_);
    }
    screen_.setVisible(wallMenu_, true);
    screen_.setModal(wallMenu_);
}

void LotUi::closeWallMenu() {
    if (wallMenu_ >= 0 && screen_.visible(wallMenu_)) {
        screen_.setVisible(wallMenu_, false);
        screen_.setModal(-1);
    }
}

void LotUi::mouseMoved(int x, int y) {
    panel_.mouseMoved(x, y);
    buyPanel_.grid().mouseAt(x, y);
    buildPanel_.grid().mouseAt(x, y);
    queue_.mouseAt(x, y);
    if (wallMenu_ < 0 || !screen_.visible(wallMenu_)) {
        return;
    }
    const int area = child(screen_, wallMenu_, kWallMenuArea);
    if (area < 0) {
        return;
    }
    const int x0 = left(area), y0 = top(area);
    if (x < x0 || y < y0 || x >= x0 + screen_.width(area) || y >= y0 + screen_.height(area)) {
        closeWallMenu();
    }
}

void LotUi::showWallMode(int mode) {
    if (puck_ < 0 || mode == wallMode_) {
        return;
    }
    wallMode_ = mode;
    for (int m = 0; m < kWallModes; ++m) {
        screen_.setVisible(child(screen_, puck_, kWallButtons + static_cast<std::uint32_t>(m)), m == mode);
    }
}

void LotUi::showStoreyLimits(bool canGoUp, bool canGoDown) {
    if (puck_ >= 0) {
        screen_.setEnabled(child(screen_, puck_, kFloorUp), canGoUp);
        screen_.setEnabled(child(screen_, puck_, kFloorDown), canGoDown);
    }
}

void LotUi::showCeilings(bool on) {
    if (puck_ >= 0) {
        screen_.setChecked(child(screen_, puck_, kToggleCeiling), on);
    }
}

LotUi::CameraButton LotUi::heldCameraButton() const {
    for (std::size_t i = 0; i < cameraButtons_.size(); ++i) {
        if (screen_.held(cameraButtons_[i])) {
            return static_cast<CameraButton>(i + 1);
        }
    }
    return CameraButton::None;
}

LotUi::Action LotUi::clicked(int window) {
    constexpr std::uint32_t kSellButton = 2;
    const std::uint32_t id = screen_.id(window);
    // The held object's panel is one button: a click sells the object, as Delete does.
    if (id == kSellButton && held_ >= 0 && screen_.visible(held_) && screen_.parent(screen_.parent(window)) == held_) {
        return Action::SellHeld;
    }
    if (const auto click = notifications_.clicked(screen_, window)) {
        if (click->close) {
            closedNotification_ = click->close;
            return Action::CloseNotification;
        }
        if (click->neighbor) {
            clickedNeighbor_ = *click->neighbor;
            return Action::CentreOnNeighbor;
        }
        return Action::None;
    }
    if (wallMenu_ >= 0 && screen_.parent(window) == wallMenu_) {
        if (id >= 1 && id <= static_cast<std::uint32_t>(kWallModes)) {
            pickedWallMode_ = static_cast<int>(id) - 1;
            closeWallMenu();
            return Action::WallMode;
        }
        return Action::None;
    }
    if (screen_.parent(window) == puck_) {
        if (id >= kWallButtons && id < kWallButtons + kWallModes) {
            openWallMenu();
            return Action::None;
        }
        if (id == kFloorUp) {
            return Action::FloorUp;
        }
        if (id == kFloorDown) {
            return Action::FloorDown;
        }
        if (id == kToggleCeiling) {
            return Action::ToggleCeiling;
        }
    }
    // Advanced Neighborhood Options opens and closes its strip. In the strip, Return goes back to the
    // neighborhood as the Neighborhood button does, and Main Menu opens the neighborhood chooser. The
    // choosers don't act yet.
    if (id == kHoodButton && hoodButtonHolder_ >= 0 && screen_.parent(window) == hoodButtonHolder_) {
        showHoodStrip(!screen_.visible(hoodStrip_));
        return Action::None;
    }
    if (hoodStrip_ >= 0 && screen_.parent(window) == hoodStrip_) {
        return id == kHoodReturn ? Action::Neighborhood : id == kHoodMainMenu ? Action::MainMenu : Action::None;
    }
    if (id == kOptions && screen_.parent(window) == puck_) {
        const bool wasLive = liveMode();
        showOptions(!optionsShown());
        return wasLive ? Action::LeftLiveMode : liveMode() ? Action::BackToLiveMode : Action::None;
    }
    // Buy and Build show their panels in the live panel's place.
    if ((id == kBuyMode || id == kBuildMode) && screen_.parent(window) == puck_) {
        const Mode mode = id == kBuyMode ? Mode::Buy : Mode::Build;
        if (mode == mode_) {
            return Action::None;
        }
        const bool wasLive = liveMode();
        setMode(mode);
        return wasLive ? Action::LeftLiveMode : Action::None;
    }
    // The hand button, while the hand holds a catalog item or another tool is in use, goes back to lifting.
    if (const SharedButtons* b = sharedButtons(); b != nullptr && window == screen_.find(mode_ == Mode::Buy ? buy_ : build_, id)) {
        if (id == b->hand && (buying_ || tool_ != Tool::Hand)) {
            return Action::HandTool;
        }
        if (id == b->design || id == b->eyedropper || id == b->sledgehammer) {
            return id == b->design ? Action::DesignTool : id == b->eyedropper ? Action::EyedropperTool : Action::SledgehammerTool;
        }
        if (id == b->dayNight) {
            return Action::DayNight;
        }
        if (id == b->topDown || id == b->ceiling) {
            return id == b->topDown ? Action::TopDownView : Action::CeilingView;
        }
        if (id == b->undo || id == b->redo) {
            return id == b->undo ? Action::Undo : Action::Redo;
        }
    }
    if (buyPanel_.clicked(window) || buildPanel_.clicked(window)) {
        return Action::None;
    }
    if (graphicsOptions_.clicked(window)) {
        return graphicsOptions_.takeChanged() ? Action::GraphicsOptionChanged : Action::None;
    }
    if (audioOptions_.clicked(window)) {
        return audioOptions_.takeChanged() ? Action::AudioOptionChanged : Action::None;
    }
    if (gameOptions_.clicked(window)) {
        return gameOptions_.takeChanged() ? Action::GameOptionChanged : Action::None;
    }
    if (id == kLiveMode && screen_.parent(window) == puck_ && !liveMode()) {
        setMode(Mode::Live);
        return Action::BackToLiveMode;
    }
    if (pauseFrame_.clicked(window)) {
        return Action::Resume;
    }
    if (petShown_ && petPanel_.clicked(window)) {
        return Action::None;
    }
    const bool rewardsWereOpen = panel_.sub() == LivePanel::Sub::Rewards;
    if (!petShown_ && panel_.clicked(window)) {
        const bool rewardsOpen = panel_.sub() == LivePanel::Sub::Rewards;
        if (rewardsOpen != rewardsWereOpen) {
            rewardsPaused_ = false;
            return rewardsOpen ? Action::RewardsOpened : Action::RewardsClosed;
        }
        return Action::None;
    }
    if (const auto sim = skewer_.clicked(window)) {
        clickedSim_ = *sim;
        return Action::SelectSim;
    }
    if (id == kNeighborhood && optionsShown()) {
        return Action::Neighborhood;
    }
    if (id == kQuit && optionsShown()) {
        return Action::Quit;
    }
    if (id == kSave && optionsShown()) {
        return Action::Save;
    }
    switch (id) {
    case kPause: return Action::Pause;
    case kNormal: return Action::Normal;
    case kHigh: return Action::High;
    case kUltra: return Action::Ultra;
    default: return Action::None;
    }
}

void LotUi::showTracked(std::int16_t sim) {
    tracked_ = sim;
    skewer_.setTracked(sim);
    // The live panel's portrait has the star too, while its Sim is the one tracked.
    const bool star = sim != 0 && sim == portraitShown_;
    if (live_ >= 0 && starShown_ != star) {
        starShown_ = star;
        screen_.setVisible(screen_.find(screen_.find(shownPanel(), kPortraitHolder), kPanelStar), star);
    }
}

LotUi::Action LotUi::rightClicked(int window, LotLife& life, bool ctrl) {
    if (ctrl) {
        for (const CatalogGrid* grid : {&buyPanel_.grid(), &buildPanel_.grid()}) {
            if (const auto guid = grid->itemAt(window)) {
                redrawPicture(*guid);
                return Action::None;
            }
        }
    }
    if (const auto want = panel_.wantAt(window)) {
        life.toggleHold(life.selected(), want->index, want->fear);
    }
    if (const auto sim = skewer_.clicked(window)) {
        clickedSim_ = *sim;
        return Action::TrackSim;
    }
    return Action::None;
}

void LotUi::show(const std::string& clock, ts2::sim::Speed speed, bool paused, std::int32_t funds) {
    if (live_ < 0) {
        return;
    }
    screen_.setText(clock_, clock);
    screen_.setText(petClock_, clock);
    screen_.setText(funds_, fundsText(funds));
    using ts2::sim::Speed;
    // The chosen speed stays pressed while paused. The pause button flips every half second.
    const auto now = std::chrono::steady_clock::now();
    if (paused && (!wasPaused_ || now - pauseBlink_ >= kPauseBlink)) {
        pauseLit_ = !wasPaused_ || !pauseLit_;
        pauseBlink_ = now;
    }
    wasPaused_ = paused;
    for (const auto& buttons : {speedButtons_, petSpeedButtons_}) {
        screen_.setChecked(buttons[0], paused && pauseLit_);
        screen_.setChecked(buttons[1], speed == Speed::Normal);
        screen_.setChecked(buttons[2], speed == Speed::High);
        screen_.setChecked(buttons[3], speed == Speed::Ultra);
    }
    pauseFrame_.show(paused);
    // The game enables the pause sign in live mode only: not in the options menu, Buy or Build, nor
    // while the inventory and rewards panel holds the game paused.
    pauseFrame_.setEnabled(liveMode() && (petShown_ || panel_.sub() != LivePanel::Sub::Rewards));
    // Unpausing while the inventory and rewards panel has the game paused closes the panel.
    if (panel_.sub() == LivePanel::Sub::Rewards) {
        if (paused) {
            rewardsPaused_ = true;
        } else if (rewardsPaused_) {
            rewardsPaused_ = false;
            panel_.closeSub();
        }
    }
}

void LotUi::showNotifications(LotLife& life, double realSeconds) {
    notificationPortraits_.collect();
    notificationSmallPortraits_.collect();
    NotificationStack::Look look;
    look.portrait = [&](std::int16_t neighbor, int side) {
        return targetPicture(side == 0 ? notificationPortraits_ : notificationSmallPortraits_, life, neighbor);
    };
    look.objectPicture = [&](std::uint32_t guid, int side) { return objectPicture(life, guid, static_cast<std::uint32_t>(side)); };
    look.neighborName = [&](std::int16_t neighbor) {
        const auto name = life.neighborName(neighbor);
        if (!name) {
            return std::string();
        }
        return app::fullName(name->first, name->last);
    };
    look.objectName = [&](std::uint32_t guid) { return life.objectName(guid); };
    look.neighborOf = [&](std::int16_t object) { return life.neighborId(object); };
    look.typeOf = [&](std::int16_t object) {
        const auto* o = life.simulation().world().find(object);
        return o != nullptr ? o->definition.guid() : 0u;
    };
    auto& world = life.simulation().world();
    for (const std::int16_t id : notifications_.update(screen_, world.notifications(), realSeconds, life.paused(), look)) {
        world.hideNotification(id);
    }
}

std::optional<ts2::tga::Pixels> LotUi::messagePicture(LotLife& life, std::int16_t neighbor, std::uint32_t objectType) {
    constexpr std::uint32_t kSide = 40;
    const ts2::tga::Pixels* found = neighbor != 0 ? targetPicture(notificationSmallPortraits_, life, neighbor)
                                    : objectType != 0 ? objectPicture(life, objectType, kSide)
                                                      : nullptr;
    if (found == nullptr || found->width == 0 || found->height == 0) {
        return std::nullopt;
    }
    return found->width == kSide && found->height == kSide ? *found : ts2::content::scaled(*found, kSide, kSide);
}

void LotUi::closeClickedNotification(LotLife& life) {
    if (closedNotification_) {
        life.simulation().world().hideNotification(*closedNotification_);
        closedNotification_.reset();
    }
}

const ts2::tga::Pixels* LotUi::targetPicture(ts2::content::TargetPictures& pictures, LotLife& life, std::int16_t neighbor) {
    if (!pictures.known(neighbor)) {
        // A household Sim on the lot has its fresh portrait. Anyone else has its character file's,
        // read in the background (asked for again next frame while too many are loading), or an
        // NPC without one its object's own.
        const auto drawn = std::find_if(portraits_.begin(), portraits_.end(), [&](const auto& p) { return life.neighborId(p.first) == neighbor; });
        if (drawn != portraits_.end()) {
            pictures.make(neighbor, drawn->second);
        } else if (const auto file = life.neighborPortrait(neighbor)) {
            pictures.load(neighbor, file->file, file->instance);
        } else if (const auto npc = life.npcPortrait(neighbor)) {
            pictures.make(neighbor, *npc);
        } else if (const auto drawnHere = drawnPortraits_.find(neighbor); drawnHere != drawnPortraits_.end()) {
            pictures.make(neighbor, drawnHere->second.value_or(ts2::tga::Pixels{}));
        } else if (life.simulation().world().nextAfter(0, [&](const ts2::sim::SimObject& o) {
                       return o.isPerson() && life.simulation().world().neighborIdOf(o) == neighbor;
                   }) != nullptr) {
            // Asked for again once it is drawn.
            if (std::ranges::find(portraitsWanted_, neighbor) == portraitsWanted_.end()) {
                portraitsWanted_.push_back(neighbor);
            }
        } else {
            pictures.make(neighbor, {});
        }
    }
    return pictures.find(neighbor);
}

// What the wants panel shows for each want, as the game's want icon picks it: the portrait of the
// Sim it is aimed at, the picture of the object (career, badge) it is about, or else its icon (a
// category's own icon first), and its badge.
std::vector<WantsPanel::Want> LotUi::panelWants(LotLife& life, const std::vector<ts2::swaf::Want>& saved) {
    using ts2::content::WantArgument;
    std::vector<WantsPanel::Want> out;
    for (const auto& w : saved) {
        WantsPanel::Want p;
        p.guid = w.guid;
        p.argument = w.argument;
        p.held = (w.flags & kHeldWant) != 0;
        if (const auto* d = life.wantDefinition(w.guid)) {
            wantPopupTexts(life, w, *d, p);
            const auto kind = static_cast<WantArgument>(w.type);
            p.primaryIcon = d->primaryIcon;
            p.power = d->level == ts2::content::WantLevel::Power;
            p.influence = d->influence != 0;
            // The badge needs the argument set (wants without an argument kind always have it).
            p.secondaryIcon = kind == WantArgument::None || w.argument != 0 ? d->secondaryIcon : 0;
            if (kind == WantArgument::Category) {
                if (const auto* category = life.wantCategory(static_cast<std::uint32_t>(w.argument))) {
                    p.primaryIcon = category->primaryIcon != 0 ? category->primaryIcon : p.primaryIcon;
                    p.secondaryIcon = category->secondaryIcon != 0 ? category->secondaryIcon : p.secondaryIcon;
                }
            } else if (kind == WantArgument::Sim && w.argument >= 1 && w.argument <= INT16_MAX && life.knowsNeighbor(static_cast<std::int16_t>(w.argument))) {
                p.hasTarget = true;
                p.targetPicture = targetPicture(targets_, life, static_cast<std::int16_t>(w.argument));
                p.popupTargetPicture = targetPicture(wantPopupPortraits_, life, static_cast<std::int16_t>(w.argument));
            } else if (kind == WantArgument::Object || kind == WantArgument::Career || kind == WantArgument::Badge) {
                if (const auto* picture = objectPicture(life, static_cast<std::uint32_t>(w.argument), kWantIconSize)) {
                    p.hasTarget = true;
                    p.targetPicture = picture;
                    p.popupTargetPicture = objectPicture(life, static_cast<std::uint32_t>(w.argument), kWantPopupPictureSize);
                }
            }
        }
        out.push_back(p);
    }
    return out;
}

// A want popup's texts, as the game's want text builder makes them: the name from the want's string
// set (row 1, or 2 for a female Sim, with the target and $Value put in, a career want's track, job
// title and level, and row 3 when there is no target, nothing to put in or a token not put in yet),
// then its aspiration and influence values. Then the description (row 4).
void LotUi::wantPopupTexts(LotLife& life, const ts2::swaf::Want& w, const ts2::content::WantDefinition& d, WantsPanel::Want& p) {
    using ts2::content::WantArgument;
    const std::uint32_t texts = ts2::hash::groupId("Wants");
    auto row = [&](std::uint32_t n) { return screen_.data().text(texts, d.stringSet << 16 | n); };
    auto replaceAll = [](std::string text, std::string_view token, const std::string& value) {
        for (auto at = text.find(token); at != std::string::npos; at = text.find(token, at + value.size())) {
            text.replace(at, token.size(), value);
        }
        return text;
    };
    std::string name;
    const auto kind = static_cast<WantArgument>(w.type);
    // $Value is the want's value times its definition's multiplier.
    const std::string valueText = ts2::content::groupedNumber(std::int64_t{w.value} * d.integerMultiplier);
    if (kind == WantArgument::Sim && w.argument >= 1 && w.argument <= INT16_MAX) {
        if (const auto neighbor = life.neighborName(static_cast<std::int16_t>(w.argument))) {
            name = row(neighbor->female ? 2 : 1);
            name = name.empty() ? row(1) : name;
            name = replaceAll(replaceAll(name, "$Neighbor", neighbor->first), "$Value", valueText);
        }
    } else if (kind == WantArgument::Object) {
        name = replaceAll(replaceAll(row(1), "$Object", life.objectName(static_cast<std::uint32_t>(w.argument))), "$Value", valueText);
    } else if (kind == WantArgument::Career && life.knowsNeighbor(static_cast<std::int16_t>(w.id))) {
        // The career object the argument names and the level the value holds. The job title is line
        // level x 2 of the career's job titles (string table 0x93), 20 lines on for a want of a
        // female Sim (person data 0x41), whose titles follow the male ones.
        constexpr std::uint16_t kJobTitles = 0x93;
        constexpr int kFemaleTitles = 20;
        auto& simulation = life.simulation();
        if (const auto* career = simulation.code().objectType(static_cast<std::uint32_t>(w.argument))) {
            const bool female = simulation.world().neighborPersonValue(static_cast<std::int16_t>(w.id), ts2::sim::kGenderPersonData) != 0;
            const int line = w.value * 2 + (female ? kFemaleTitles : 0);
            const std::string title = line >= 1 ? simulation.code().string(career->group, kJobTitles, static_cast<std::size_t>(line - 1)).value_or("") : "";
            name = replaceAll(row(1), "$CareerTrack", life.objectName(static_cast<std::uint32_t>(w.argument)));
            name = replaceAll(replaceAll(name, "$JobTitle", title), "$Level", ts2::content::groupedNumber(w.value));
        }
    } else if (kind == WantArgument::None) {
        // Row 1 only when it has a value to put in. $Money isn't put in yet.
        name = row(1).find("$Value") == std::string::npos ? std::string() : replaceAll(row(1), "$Value", valueText);
    }
    if (name.empty() || name.find('$') != std::string::npos) {
        name = row(3);
        name = name.empty() ? row(1) : name;
    }
    // The values: entries 0xB "Aspiration Value" and 8 "Influence Value" of Live text table 0x9F.
    constexpr std::uint32_t kLiveText = 0x7F96C284, kValues = 0x9F;
    auto value = [&](std::uint32_t label, std::int32_t v) {
        return "\n" + screen_.data().text(kLiveText, kValues << 16 | label) + (v < 0 ? " " : " +") + ts2::content::groupedNumber(v);
    };
    // The icon's tooltip: the name, then a line for each value that isn't 0, the number first ("+" when
    // above 0) and then entry 0xE "Aspiration" or 0xD "Influence" of the same table.
    auto tipValue = [&](std::uint32_t label, std::int32_t v) {
        return "\n" + std::string(v > 0 ? "+" : "") + ts2::content::groupedNumber(v) + " " + screen_.data().text(kLiveText, kValues << 16 | label);
    };
    // Wants saved before version 6 have no aspiration value and those before version 9 no influence.
    // The game takes them from the want's score and influence.
    const std::int32_t aspiration = w.tail.size() > 1 ? w.tail[1] : d.score;
    const std::int32_t influence = w.tail.size() > 2 ? w.tail[2] : d.influence;
    p.tip = name;
    // (Rows 0xB and 8, counted from 0.)
    if (aspiration != 0) {
        p.tip += tipValue(0xE, aspiration);
        name += value(0xC, aspiration);
    }
    if (influence != 0) {
        p.tip += tipValue(0xD, influence);
        name += value(0x9, influence);
    }
    p.popupName = name;
    p.description = row(4);
}

std::optional<ts2::tga::Pixels> LotUi::objectImage(LotLife& life, std::uint32_t guid, std::uint32_t named) {
    // The game's thumbnail service: image (the one named, else OBJD field 0x32, else the model's
    // index, else 1) of the object's group. Objects without that image are rendered in 3D and
    // cached, filed by the object's model name (list 0x85 entry at the named index, else at the
    // model index, the 32-bit value in fields 0x48 and 0x49). Those the cache lacks are drawn here.
    constexpr std::size_t kThumbnailField = 0x32, kModelIndexLow = 0x48, kModelIndexHigh = 0x49;
    constexpr std::uint16_t kModelNames = 0x85;
    constexpr std::uint32_t kMaxPictureSide = 512;
    auto& code = life.simulation().code();
    const auto* type = code.objectType(guid);
    if (type == nullptr) {
        return std::nullopt;
    }
    const auto& d = type->definition;
    const std::uint32_t model = d.field(kModelIndexLow) | static_cast<std::uint32_t>(d.field(kModelIndexHigh)) << 16;
    const std::uint32_t instance = named != 0 ? named : d.field(kThumbnailField) != 0 ? d.field(kThumbnailField) : model != 0 ? model : 1;
    auto image = code.image(type->group, instance, kMaxPictureSide);
    if (image) {
        return image;
    }
    if (!objectThumbnails_) {
        objectThumbnails_.emplace(objectThumbnailsFile_);
    }
    if (named != 0) {
        const auto name = code.string(type->group, kModelNames, named);
        return name ? objectThumbnails_->picture(type->group, *name, kMaxPictureSide) : std::nullopt;
    }
    // A picture drawn this run is newer than the cache's, and one asked to be drawn again skips it.
    if (const auto drawn = drawnPictures_.find(guid); drawn != drawnPictures_.end()) {
        return drawn->second;
    }
    const auto name = code.string(type->group, kModelNames, model);
    if (name && !redraw_.contains(guid)) {
        image = objectThumbnails_->picture(type->group, *name, kMaxPictureSide);
    }
    // The game draws the picture of an object that has neither, and keeps it in the cache.
    if (!image) {
        const auto key = name ? ts2::content::ObjectThumbnails::key(type->group, *name) : std::nullopt;
        picturesToDraw_.try_emplace(guid, PictureToDraw{guid, {}, {}, {}, type->group, key.value_or(0)});
    }
    return image;
}

void LotUi::redrawPicture(std::uint32_t guid) {
    drawnPictures_.erase(guid);
    std::erase_if(objectPictures_, [&](const auto& entry) { return std::get<0>(entry.first) == guid; });
    redraw_.insert(guid);
}

std::optional<LotUi::PictureToDraw> LotUi::pictureToDraw() {
    if (previewToDraw_) {
        return std::exchange(previewToDraw_, std::nullopt);
    }
    if (!picturesToDraw_.empty()) {
        auto asked = std::move(picturesToDraw_.begin()->second);
        picturesToDraw_.erase(picturesToDraw_.begin());
        return asked;
    }
    if (!coloursToDraw_.empty()) {
        auto asked = std::move(coloursToDraw_.begin()->second);
        coloursToDraw_.erase(coloursToDraw_.begin());
        return asked;
    }
    return std::nullopt;
}

void LotUi::setDrawnPicture(const PictureToDraw& asked, std::optional<ts2::tga::Pixels> picture) {
    constexpr std::uint32_t kDescriptionPictureSide = 117;
    if (asked.preview) {
        if (picture && previewGrid_ != nullptr) {
            previewPicture_ = fitted(*picture, kDescriptionPictureSide, kDescriptionPictureSide);
            previewGrid_->showDescriptionPicture(&*previewPicture_);
        }
        return;
    }
    // Its sizes made before it was drawn are made again.
    if (asked.guid != 0) {
        redraw_.erase(asked.guid);
        if (picture && asked.key != 0 && objectThumbnails_) {
            objectThumbnails_->store(asked.group, asked.key, *picture);
        }
        drawnPictures_[asked.guid] = std::move(picture);
        std::erase_if(objectPictures_, [&](const auto& entry) { return std::get<0>(entry.first) == asked.guid; });
        drawnNow_.insert(asked.guid);
        return;
    }
    if (picture && colourThumbnails_) {
        colourThumbnails_->store(asked.group, asked.key, *picture);
    }
    drawnColours_[{asked.group, asked.key}] = std::move(picture);
    std::erase_if(colourPictures_, [&](const auto& entry) {
        return std::get<0>(entry.first) == asked.group && std::get<1>(entry.first) == asked.key;
    });
    coloursDrawn_ = true;
}

const ts2::tga::Pixels* LotUi::objectPicture(LotLife& life, std::uint32_t guid, std::uint32_t width, std::uint32_t height,
                                             std::uint32_t named) {
    height = height == 0 ? width : height;
    const std::uint32_t key = width << 16 | height;
    auto it = objectPictures_.find({guid, key, named});
    if (it == objectPictures_.end()) {
        std::optional<ts2::tga::Pixels> picture;
        if (const auto image = objectImage(life, guid, named)) {
            picture = fitted(*image, width, height);
        }
        it = objectPictures_.emplace(std::tuple{guid, key, named}, std::move(picture)).first;
    }
    return it->second ? &*it->second : nullptr;
}

// Balloons turn on the thumbnail service's scaling, so their pictures fill the 128 pixels whatever their size.
std::optional<ts2::tga::Pixels> LotUi::balloonObjectPicture(LotLife& life, std::uint32_t guid, std::uint32_t index) {
    const auto image = objectImage(life, guid, index);
    return image ? std::optional(ts2::content::scaled(*image, kBalloonPictureSide, kBalloonPictureSide)) : std::nullopt;
}

std::optional<ts2::tga::Pixels> LotUi::balloonMemoryPicture(LotLife& life, std::uint32_t guid) {
    constexpr std::uint16_t kMemoryType = 0x0F;
    constexpr std::uint32_t kPicture = 1, kMaxPictureSide = 256;
    auto& code = life.simulation().code();
    const auto* type = code.objectType(guid);
    if (type == nullptr || type->definition.field(ts2::objd::field::kType) != kMemoryType) {
        return std::nullopt;
    }
    const auto image = code.image(type->group, kPicture, kMaxPictureSide);
    return image ? std::optional(ts2::content::scaled(*image, kBalloonPictureSide, kBalloonPictureSide)) : std::nullopt;
}

LotUi::BalloonWant LotUi::balloonWant(LotLife& life, const ts2::swaf::Want& want) {
    using ts2::content::WantArgument;
    constexpr std::uint32_t kImages = 0x499DB772;
    const auto shown = panelWants(life, {want});
    if (shown.empty()) {
        return {};
    }
    const WantsPanel::Want& p = shown.front();
    BalloonWant out;
    const auto kind = static_cast<WantArgument>(want.type);
    if (kind == WantArgument::Sim && p.hasTarget) {
        out.neighbor = static_cast<std::int16_t>(want.argument);
        const auto* neighbor = life.simulation().world().neighbor(out.neighbor);
        out.name = std::format("BalloonEffectThumbnail_WantFear_0x{:08x}", neighbor != nullptr ? neighbor->guid : 0);
    } else if (p.hasTarget) {
        const auto guid = static_cast<std::uint32_t>(want.argument);
        if (const auto image = objectImage(life, guid, 0)) {
            out.picture = ts2::content::scaled(*image, kBalloonPictureSide, kBalloonPictureSide);
        }
        out.name = std::format("BalloonEffectThumbnail_WantFear_0x{:08x}", guid);
    } else {
        if (const auto image = screen_.data().image(kImages, p.primaryIcon)) {
            out.picture = ts2::content::scaled(*image, kBalloonPictureSide, kBalloonPictureSide);
        }
        out.name = std::format("BalloonEffectThumbnail_WantFear_0x{:08x}_0x{:08x}", kImages, p.primaryIcon);
    }
    if (const auto badge = screen_.data().image(kImages, p.secondaryIcon)) {
        out.badge = *badge;
        out.name += std::format("_0x{:08x}_0x{:08x}", kImages, p.secondaryIcon);
    }
    return out;
}

std::optional<ts2::tga::Pixels> LotUi::balloonPortrait(LotLife& life, std::int16_t neighbor) {
    constexpr std::uint32_t kImages = 0x499DB772, kBalloonShape = 0x4BBD01B6;
    const auto* picture = targetPicture(balloonPortraits_, life, neighbor);
    if (picture == nullptr) {
        return std::nullopt;
    }
    if (!balloonShape_) {
        balloonShape_ = screen_.data().image(kImages, kBalloonShape).value_or(ts2::tga::Pixels{});
    }
    return ts2::content::fadedToShape(*picture, *balloonShape_);
}

const ts2::tga::Pixels* LotUi::careerIcon(LotLife& life, std::uint32_t guid) {
    // A career reward names its career (OBJD fields 0x34 and 0x35, its GUID). The icon is the
    // career's picture 5.
    constexpr std::size_t kCareerLow = 0x34, kCareerHigh = 0x35;
    constexpr std::uint32_t kIconPicture = 5, kMaxPictureSide = 256;
    auto it = careerIcons_.find(guid);
    if (it == careerIcons_.end()) {
        std::optional<ts2::tga::Pixels> picture;
        auto& code = life.simulation().code();
        if (const auto* reward = code.objectType(guid)) {
            const std::uint32_t careerGuid = static_cast<std::uint32_t>(reward->definition.field(kCareerLow)) |
                                             static_cast<std::uint32_t>(reward->definition.field(kCareerHigh)) << 16;
            const auto* career = careerGuid != 0 ? code.objectType(careerGuid) : nullptr;
            if (career != nullptr) {
                if (auto image = code.image(career->group, kIconPicture, kMaxPictureSide)) {
                    picture = fitted(*image, kCareerIconSide, kCareerIconSide);
                }
            }
        }
        it = careerIcons_.emplace(guid, std::move(picture)).first;
    }
    return it->second ? &*it->second : nullptr;
}

std::string LotUi::catalogRatings(const ts2::objd::ObjectDefinition& d) {
    // A rating's text (UI text table 0xA0 or Live text table 0x82, by row) and where its value comes
    // from: an object data field, or one bit of a field (the line shows when the bit is set).
    struct Rating {
        std::uint32_t text;
        std::size_t field;
        std::uint16_t bit;  // 0: the field's value itself
    };
    constexpr std::uint32_t kLiveText = 0x7F96C284;
    constexpr std::uint32_t kRatings = 0xA0, kPets = 0x82, kPanelText = 0x91, kRewardNote = 0x23;
    auto row = [](std::uint32_t table, std::uint32_t n) { return table << 16 | (n + 1); };
    // In the game's order: the needs (hunger to fun, OBJD 0x52-0x57), the pets' scratch and chew
    // (0x67, 0x68), environment (0x58), the skills and cleaning (OBJD 0x59 bits) and who can use it
    // (OBJD 0x11 bits).
    static constexpr Rating kList[] = {
        {0, 0x52, 0},  {1, 0x53, 0},  {2, 0x54, 0},  {3, 0x55, 0},  {4, 0x56, 0},  {5, 0x57, 0},
        {0x22, 0x67, 0}, {0x24, 0x68, 0}, {6, 0x58, 0},
        {7, 0x59, 0x01},  {8, 0x59, 0x02},  {9, 0x59, 0x04},  {0xA, 0x59, 0x08}, {0xB, 0x59, 0x10}, {0xC, 0x59, 0x20}, {0xD, 0x59, 0x40},
        {0x10, 0x11, 0x01}, {0xF, 0x11, 0x02}, {0xE, 0x11, 0x04}, {0x11, 0x11, 0x08}, {0x12, 0x11, 0x10}, {0x13, 0x11, 0x20},
        {0x14, 0x59, 0x80},
    };
    std::string out;
    for (const auto& r : kList) {
        const auto raw = d.field(r.field);
        const int value = r.bit != 0 ? ((raw & r.bit) != 0 ? 1 : 0) : static_cast<std::int16_t>(raw);
        if (value == 0) {
            continue;
        }
        // The pets' two lines are a label with the value after it. The others are a format with
        // the value in place of %d, or none.
        const bool pet = r.field == 0x67 || r.field == 0x68;
        std::string line = screen_.data().text(pet ? kLiveText : kUiText, row(pet ? kPets : kRatings, r.text));
        if (pet) {
            line += ": " + std::to_string(value) + " ";
        } else if (const auto at = line.find("%d"); at != std::string::npos) {
            line.replace(at, 2, std::to_string(value));
        }
        out += line + "\n";
    }
    // Aspiration rewards (function sort 0x400) add a warning after a blank line.
    if ((d.field(kRewardKind) & kAspirationReward) != 0) {
        if (!out.empty()) {
            out += "\n";
        }
        out += screen_.data().text(kLiveText, row(kPanelText, kRewardNote));
    }
    return out;
}

namespace {

// A part of a multi-tile object, not its master.
bool isPart(const ts2::sim::ObjectType& type) {
    return type.definition.field(ts2::objd::field::kMultiTileMasterId) != 0 && type.definition.field(ts2::objd::field::kMultiTileSubIndex) != 0xFFFF;
}

} // namespace

void LotUi::showCatalogs(LotLife& life) {
    // Buy's sort fields: room sort flags (OBJD 0x27), function sort flags (0x28) and function subsort
    // (0x5E). Build's: build type (0x45) and build subsort (0x4A). An item with field 0x10 set is
    // listed only when its GUID is in a list not known yet.
    constexpr std::size_t kRooms = 0x27, kFunctions = 0x28, kSubsort = 0x5E, kBuildType = 0x45, kBuildSubsort = 0x4A,
                          kListedField = 0x10;
    auto& code = life.simulation().code();
    if (mode_ == Mode::Buy && !buyPanel_.hasCatalog()) {
        // On a lived-in lot the function sort lists only items with a room sort. Parts of larger
        // objects aren't listed, even those with sort fields of their own.
        std::vector<BuyPanel::Entry> entries;
        for (const std::uint32_t guid : code.objectTypeGuids()) {
            const auto* type = code.objectType(guid);
            if (type == nullptr || isPart(*type)) {
                continue;
            }
            const auto& d = type->definition;
            if (d.field(kRooms) != 0 && d.field(kFunctions) != 0 && d.field(kListedField) == 0) {
                entries.push_back({guid, d.field(kFunctions), d.field(kSubsort), d.field(kRooms), d.field(ts2::objd::field::kPrice)});
            }
        }
        // Cheapest first, as the game lists. Items of the same price may come in another order than the game's.
        std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.price < b.price; });
        buyPanel_.setCatalog(std::move(entries));
    }
    if (mode_ == Mode::Build && !buildPanel_.hasCatalog()) {
        std::vector<std::pair<int, BuildPanel::Entry>> priced;
        for (const std::uint32_t guid : code.objectTypeGuids()) {
            const auto* type = code.objectType(guid);
            if (type == nullptr || isPart(*type)) {
                continue;
            }
            const auto& d = type->definition;
            if (d.field(kBuildType) != 0 && d.field(kListedField) == 0) {
                priced.push_back({d.field(ts2::objd::field::kPrice), {guid, d.field(kBuildType), d.field(kBuildSubsort)}});
            }
        }
        // Cheapest first, as the game lists.
        std::stable_sort(priced.begin(), priced.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<BuildPanel::Entry> entries;
        for (const auto& [price, entry] : priced) {
            entries.push_back(entry);
        }
        buildPanel_.setCatalog(std::move(entries));
    }
    // Each is read once, so an install without them doesn't read every package again each frame.
    // Fireplaces are read in either mode, as Buy mode's eyedropper opens Build mode on one.
    if (!fireplacesLoaded_ && !products_.empty()) {
        fireplacesLoaded_ = true;
        loadFireplaces(code);
    }
    if (mode_ == Mode::Build && !wallToolsLoaded_ && !buildPanel_.hasTools() && !products_.empty()) {
        wallToolsLoaded_ = true;
        loadWallTools();
    }
    // Buy mode's eyedropper names floors and walls too.
    const bool coveringsWanted = mode_ == Mode::Build || (mode_ == Mode::Buy && tool_ == Tool::Eyedropper);
    if (coveringsWanted && !coveringsLoaded_ && !buildPanel_.hasCoverings() && !install_.empty()) {
        coveringsLoaded_ = true;
        if (coverings_.empty()) {
            coverings_ = ts2::content::loadCoverings(products_);
            for (std::size_t i = 0; i < coverings_.size(); ++i) {
                const auto& c = coverings_[i];
                const bool wall = c.kind == ts2::content::CoveringKind::Wall;
                std::string file = c.filename;
                std::transform(file.begin(), file.end(), file.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                coveringByGuid_.try_emplace({wall, c.guid}, i);
                coveringByFile_.try_emplace({wall, std::move(file)}, i);
            }
        }
        if (!coveringsAhead_.valid()) {
            std::vector<std::pair<std::uint32_t, ts2::content::CoveringKind>> wanted;
            for (const auto& c : coverings_) {
                wanted.emplace_back(c.guid, c.kind);
            }
            coveringsAhead_ = std::async(std::launch::async, [file = coveringThumbnailsFile_, wanted = std::move(wanted)] {
                constexpr std::uint32_t kMaxPictureSide = 512;
                std::map<std::uint32_t, ts2::tga::Pixels> out;
                ts2::content::CoveringColors cache{file};
                for (const auto& [guid, kind] : wanted) {
                    if (const auto image = cache.picture(kind, ts2::content::coveringPictureName(guid), kMaxPictureSide)) {
                        out.emplace(guid, fitted(*image, kCoveringPictureSide, kCoveringPictureSide));
                    }
                }
                return out;
            });
        }
        std::vector<BuildPanel::CoveringEntry> entries;
        for (const auto& c : coverings_) {
            entries.push_back({c.guid, c.kind == ts2::content::CoveringKind::Wall, c.subsort});
        }
        buildPanel_.setCoverings(std::move(entries));
    }
    if (mode_ == Mode::Build && !halfWallsLoaded_ && !install_.empty()) {
        halfWallsLoaded_ = true;
        halfWalls_ = ts2::content::loadHalfWalls(products_);
        std::vector<std::uint32_t> guids;
        for (const auto& h : halfWalls_) {
            guids.push_back(h.guid);
        }
        buildPanel_.setHalfWalls(std::move(guids));
    }
    if (eyedropShown_ != 0 && !buyOrBuild()) {
        eyedropShown_ = 0;
    } else if (eyedropShown_ != 0 && (mode_ == Mode::Buy ? buyPanel_.hasCatalog() : buildPanel_.hasCatalog())) {
        if (mode_ == Mode::Buy) {
            buyPanel_.showItem(eyedropShown_);
        } else {
            buildPanel_.showItem(eyedropShown_);
        }
        eyedropShown_ = 0;
    }
    if (mode_ == Mode::Buy) {
        showCatalog(life, buyPanel_.grid());
    } else if (mode_ == Mode::Build && buildPanel_.listing() == BuildPanel::Listing::Tools) {
        showWallToolCatalog(buildPanel_.grid());
    } else if (mode_ == Mode::Build && buildPanel_.listing() == BuildPanel::Listing::HalfWalls) {
        showHalfWallCatalog(buildPanel_.grid());
    } else if (mode_ == Mode::Build && buildPanel_.listing() == BuildPanel::Listing::Objects) {
        showCatalog(life, buildPanel_.grid());
    } else if (mode_ == Mode::Build) {
        showCoveringCatalog(buildPanel_.grid(), buildPanel_.listing() == BuildPanel::Listing::Walls);
    }
    // Colour pictures drawn since the Design dialog opened take the place of its empty swatches.
    if (coloursDrawn_ && designDialog_.isOpen()) {
        designDialog_.showPictures(designSwatches(life));
    }
    coloursDrawn_ = false;
    drawnNow_.clear();
}

void LotUi::loadWallTools() {
    // The game's building tips (Build text table 0x91): a clone's tip, counted from 1, ends its description.
    constexpr std::uint32_t kBuildingTips = 0x91;
    constexpr std::uint32_t kPictureSide = 40, kDescriptionPictureSide = 117;
    const auto scripts = ts2::content::loadBuildScripts(products_);
    auto& data = screen_.data();
    if (!scripts.platformTextFile.empty()) {
        const auto texts = data.texts(ts2::hash::groupId(scripts.platformTextFile), scripts.platformTextTable);
        for (const auto& [product, platform] : scripts.platforms) {
            const auto at = static_cast<std::size_t>(platform.textIndex) * 3;
            if (platform.textIndex >= 0 && at < texts.size()) {
                platformPrices_[product] = static_cast<std::int32_t>(std::strtol(texts[at].c_str(), nullptr, 10));
            }
        }
    }
    if (scripts.wallTextFile.empty()) {
        return;
    }
    const std::uint32_t group = ts2::hash::groupId(scripts.wallTextFile);
    const auto texts = data.texts(group, scripts.wallTextTable);
    const auto tips = data.texts(group, kBuildingTips);
    for (const auto& [id, style] : scripts.wallStyles) {
        for (std::size_t c = 0; c < style.clones.size(); ++c) {
            const auto& clone = style.clones[c];
            const auto at = static_cast<std::size_t>(clone.textIndex) * 3;
            if (clone.textIndex < 0 || at + 2 >= texts.size()) {
                continue;
            }
            WallToolEntry entry;
            entry.choice = {id, static_cast<int>(c), static_cast<std::int32_t>(std::strtol(texts[at].c_str(), nullptr, 10))};
            entry.name = texts[at + 1];
            entry.description = texts[at + 2];
            if (clone.tip > 0 && static_cast<std::size_t>(clone.tip) <= tips.size()) {
                entry.description += "\n\n" + tips[static_cast<std::size_t>(clone.tip) - 1];
            }
            if (const auto image = data.image(clone.thumbnailGroup, clone.thumbnailInstance)) {
                entry.picture = fitted(*image, kPictureSide, kPictureSide);
                entry.bigPicture = fitted(*image, kDescriptionPictureSide, kDescriptionPictureSide);
            }
            wallTools_.push_back(std::move(entry));
        }
    }
    std::vector<std::uint32_t> ids;
    for (std::size_t i = 0; i < wallTools_.size(); ++i) {
        ids.push_back(static_cast<std::uint32_t>(i + 1));
    }
    buildPanel_.setTools(std::move(ids));
}

void LotUi::loadFireplaces(ts2::sim::CodeSource& code) {
    // The game's building tips (Build text table 0x91), as loadWallTools.
    constexpr std::uint32_t kBuildingTips = 0x91;
    constexpr std::uint32_t kPictureSide = 40, kDescriptionPictureSide = 117, kMaxPictureSide = 512;
    const auto scripts = ts2::content::loadBuildScripts(products_);
    std::map<std::string, std::int32_t> wanted;  // lowercase mantle name: fireplace id
    for (const auto& [id, fireplace] : scripts.fireplaces) {
        if (!fireplace.mantle.empty()) {
            wanted.emplace(ts2::scene::lower(fireplace.mantle), id);
        }
    }
    std::map<std::int32_t, std::uint32_t> mantles;  // by fireplace id
    for (const std::uint32_t guid : code.objectTypeGuids()) {
        const auto* type = code.objectType(guid);
        if (type == nullptr) {
            continue;
        }
        if (const auto it = wanted.find(ts2::scene::lower(type->definition.name)); it != wanted.end()) {
            mantles.try_emplace(it->second, guid);
        }
    }
    auto& data = screen_.data();
    ts2::content::CoveringColors pictures{coveringThumbnailsFile_};
    std::vector<std::pair<std::int32_t, std::uint32_t>> priced;
    for (const auto& [id, guid] : mantles) {
        const auto& fireplace = scripts.fireplaces.at(id);
        const std::uint32_t group = ts2::hash::groupId(fireplace.textFile);
        const auto texts = data.texts(group, fireplace.textTable);
        const auto at = static_cast<std::size_t>(fireplace.textIndex) * 3;
        if (fireplace.textIndex < 0 || at + 2 >= texts.size()) {
            continue;
        }
        // The catalog lists and charges the mantle's own price (OBJD 0x12). The text's price differs
        // for some (the Shoddy Fireplace's text says 417, its mantle 100), and the game's red cells
        // and order follow the mantle's.
        FireplaceText text{texts[at + 1], texts[at + 2], code.objectType(guid)->definition.field(ts2::objd::field::kPrice)};
        // The game's own picture of the whole fireplace, cached as it draws one. The mantle's
        // picture stands in when none is cached, as drawing one isn't done yet.
        if (const auto image = pictures.fireplacePicture(id, kMaxPictureSide)) {
            text.picture = fitted(*image, kPictureSide, kPictureSide);
            text.bigPicture = fitted(*image, kDescriptionPictureSide, kDescriptionPictureSide);
        }
        const auto tips = data.texts(group, kBuildingTips);
        if (fireplace.tip > 0 && static_cast<std::size_t>(fireplace.tip) <= tips.size()) {
            text.description += "\n\n" + tips[static_cast<std::size_t>(fireplace.tip) - 1];
        }
        priced.emplace_back(text.price, guid);
        fireplaces_[guid] = std::move(text);
    }
    // Cheapest first, as the game lists. Fireplaces of the same price may come in another order than the game's.
    std::stable_sort(priced.begin(), priced.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::uint32_t> listed;
    for (const auto& [price, guid] : priced) {
        listed.push_back(guid);
    }
    buildPanel_.setFireplaces(std::move(listed));
}

std::optional<LotUi::WallToolChoice> LotUi::wallToolChoice() const {
    if (mode_ == Mode::Build && buildPanel_.listing() == BuildPanel::Listing::HalfWalls) {
        const auto it = std::find_if(halfWalls_.begin(), halfWalls_.end(), [&](const auto& h) { return h.guid == halfWallChosen_; });
        if (it == halfWalls_.end()) {
            return std::nullopt;
        }
        return WallToolChoice{it->guid, 0, it->price};
    }
    if (wallToolChosen_ >= wallTools_.size()) {
        return std::nullopt;
    }
    return wallTools_[wallToolChosen_].choice;
}

std::int32_t LotUi::platformPrice(std::int32_t product) const {
    const auto found = platformPrices_.find(product);
    return found != platformPrices_.end() ? found->second : 0;
}

std::int32_t LotUi::wallStylePrice(std::uint32_t style) const {
    for (const auto& h : halfWalls_) {
        if (h.guid == style) {
            return h.price;
        }
    }
    for (const auto& e : wallTools_) {
        if (e.choice.style == style) {
            return e.choice.price;
        }
    }
    return 0;
}

void LotUi::showWallToolCatalog(CatalogGrid& grid) {
    if (const auto first = grid.takeFirstPicked(); first && *first >= 1 && *first <= wallTools_.size()) {
        wallToolChosen_ = *first - 1;
    }
    if (const auto chosen = grid.takeChosen(); chosen && *chosen >= 1 && *chosen <= wallTools_.size()) {
        wallToolChosen_ = *chosen - 1;
        wallToolClicked_ = true;
        const auto& e = wallTools_[wallToolChosen_];
        grid.showDescription({e.name, fundsText(e.choice.price), e.description, "", e.bigPicture ? &*e.bigPicture : nullptr});
    }
    grid.choose(static_cast<std::uint32_t>(wallToolChosen_ + 1));
    grid.showPictures([&](std::uint32_t id) -> const ts2::tga::Pixels* {
        return id >= 1 && id <= wallTools_.size() && wallTools_[id - 1].picture ? &*wallTools_[id - 1].picture : nullptr;
    });
}

void LotUi::showCoveringCatalog(CatalogGrid& grid, bool walls) {
    constexpr std::uint32_t kPictureSide = kCoveringPictureSide, kDescriptionPictureSide = 117;
    constexpr int kPicturesPerFrame = 4;
    if (coveringsAhead_.valid() && coveringsAhead_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        for (auto& [guid, picture] : coveringsAhead_.get()) {
            coveringImages_.try_emplace({guid, kPictureSide}, std::move(picture));
        }
    }
    // A covering picked on opening the category starts the floor or wall paint tool with it.
    if (const auto first = grid.takeFirstPicked()) {
        (walls ? wallCoveringChosen_ : floorChosen_) = *first;
        (walls ? wallCoveringPicked_ : floorPicked_) = true;
    }
    if (const auto chosen = grid.takeChosen()) {
        const auto it = std::find_if(coverings_.begin(), coverings_.end(), [&](const auto& c) { return c.guid == *chosen; });
        if (it != coverings_.end()) {
            grid.showDescription({it->name, fundsText(it->cost), it->description, "", coveringPicture(it->guid, walls, kDescriptionPictureSide)});
            (walls ? wallCoveringChosen_ : floorChosen_) = it->guid;
            (walls ? wallCoveringPicked_ : floorPicked_) = true;
        }
    }
    int budget = kPicturesPerFrame;
    grid.showPictures([&](std::uint32_t guid) -> const ts2::tga::Pixels* {
        if (!coveringImages_.contains({guid, kPictureSide}) && budget-- <= 0) {
            return nullptr;
        }
        return coveringPicture(guid, walls, kPictureSide);
    });
}

const ts2::tga::Pixels* LotUi::coveringPicture(std::uint32_t guid, bool wall, std::uint32_t side) {
    constexpr std::uint32_t kMaxPictureSide = 512;
    auto it = coveringImages_.find({guid, side});
    if (it == coveringImages_.end()) {
        if (!coveringPictures_) {
            coveringPictures_.emplace(coveringThumbnailsFile_);
        }
        std::optional<ts2::tga::Pixels> picture;
        const auto kind = wall ? ts2::content::CoveringKind::Wall : ts2::content::CoveringKind::Floor;
        const std::string name = ts2::content::coveringPictureName(guid);
        if (const auto image = coveringPictures_->picture(kind, name, kMaxPictureSide)) {
            picture = fitted(*image, side, side);
        } else if (const auto texture = coveringTextures_ ? coveringTextures_(guid, wall) : std::nullopt) {
            // The game keeps a picture it draws in the cache, where the room light finds its colour.
            const auto drawn = ts2::content::coveringThumbnail(*texture, kind);
            coveringPictures_->store(kind, name, drawn);
            picture = fitted(drawn, side, side);
        }
        it = coveringImages_.emplace(std::pair{guid, side}, std::move(picture)).first;
    }
    return it->second ? &*it->second : nullptr;
}

void LotUi::showHalfWallCatalog(CatalogGrid& grid) {
    constexpr std::uint32_t kPictureSide = 40, kDescriptionPictureSide = 117;
    constexpr int kPicturesPerFrame = 4;
    if (const auto first = grid.takeFirstPicked()) {
        halfWallChosen_ = *first;
    }
    if (const auto chosen = grid.takeChosen()) {
        const auto it = std::find_if(halfWalls_.begin(), halfWalls_.end(), [&](const auto& h) { return h.guid == *chosen; });
        if (it != halfWalls_.end()) {
            halfWallChosen_ = it->guid;
            wallToolClicked_ = true;
            grid.showDescription({it->name, fundsText(it->price), it->description, "", halfWallPicture(it->guid, kDescriptionPictureSide)});
        }
    }
    // Until one is picked the first is chosen.
    if (halfWallChosen_ == 0 && !halfWalls_.empty()) {
        halfWallChosen_ = halfWalls_.front().guid;
    }
    grid.choose(halfWallChosen_);
    int budget = kPicturesPerFrame;
    grid.showPictures([&](std::uint32_t guid) -> const ts2::tga::Pixels* {
        if (!halfWallImages_.contains({guid, kPictureSide}) && budget-- <= 0) {
            return nullptr;
        }
        return halfWallPicture(guid, kPictureSide);
    });
}

const ts2::tga::Pixels* LotUi::halfWallPicture(std::uint32_t guid, std::uint32_t side) {
    constexpr std::uint32_t kMaxPictureSide = 512;
    auto it = halfWallImages_.find({guid, side});
    if (it == halfWallImages_.end()) {
        if (!coveringPictures_) {
            coveringPictures_.emplace(coveringThumbnailsFile_);
        }
        std::optional<ts2::tga::Pixels> picture;
        if (const auto image = coveringPictures_->fencePicture(guid, kMaxPictureSide)) {
            picture = fitted(*image, side, side);
        }
        it = halfWallImages_.emplace(std::pair{guid, side}, std::move(picture)).first;
    }
    return it->second ? &*it->second : nullptr;
}

void LotUi::showCatalog(LotLife& life, CatalogGrid& grid) {
    constexpr std::uint32_t kPictureSide = 40, kDescriptionPictureSide = 117;
    // The shown cells' pictures are read within one frame where they can be, then time left reads ahead
    // so other pages open ready.
    constexpr auto kShownBudget = std::chrono::milliseconds(12), kAheadBudget = std::chrono::milliseconds(2);
    auto& code = life.simulation().code();
    // An item picked on opening a Build mode category goes into the hand without its description.
    if (const auto first = grid.takeFirstPicked()) {
        bought_ = *first;
        boughtFrom_ = &grid;
    }
    // A clicked item's description: its catalog name and text (catalog strings 0 and 1) and price.
    if (const auto chosen = grid.takeChosen()) {
        // Build mode's catalog buys its doors, windows and other objects as Buy mode's does.
        bought_ = *chosen;
        boughtFrom_ = &grid;
        if (const auto fireplace = fireplaces_.find(*chosen); fireplace != fireplaces_.end() && code.objectType(*chosen) != nullptr) {
            // A fireplace shows the texts of its catalog script, not its mantle's own.
            const auto& text = fireplace->second;
            described_[&grid] = *chosen;
            grid.showDescription({text.name, fundsText(text.price), text.description, catalogRatings(code.objectType(*chosen)->definition),
                                  text.bigPicture ? &*text.bigPicture : objectPicture(life, *chosen, kDescriptionPictureSide),
                                  colourStrips(life, *chosen, &grid == boughtFrom_)});
        } else if (const auto* type = code.objectType(*chosen)) {
            const auto set = static_cast<std::uint16_t>(type->definition.field(ts2::objd::field::kCatalogStringsId));
            described_[&grid] = *chosen;
            grid.showDescription({code.catalogString(type->group, set, 0).value_or(""), fundsText(type->definition.field(ts2::objd::field::kPrice)),
                                  code.catalogString(type->group, set, 1).value_or(""), catalogRatings(type->definition),
                                  objectPicture(life, *chosen, kDescriptionPictureSide), colourStrips(life, *chosen, &grid == boughtFrom_)});
        }
    }
    // With two strips, the description picture shows the colours picked in both.
    if (grid.takePreviewPicked()) {
        const auto shown = described_.find(&grid);
        const auto strips = shown != described_.end() ? shownStrips_.find(shown->second) : shownStrips_.end();
        const auto chosen = grid.chosenColours();
        if (strips != shownStrips_.end() && strips->second.strips.size() == 2 && chosen.size() == 2) {
            PictureToDraw asked;
            asked.preview = true;
            for (std::size_t k = 0; k < 2; ++k) {
                const auto& strip = strips->second.strips[k];
                if (chosen[k] >= strip.choices.size()) {
                    break;
                }
                const auto& choice = strip.choices[chosen[k]];
                const auto own = strips->second.models.find(strip.subset);
                const std::string model = own != strips->second.models.end() ? own->second : choice.model;
                if (k == 0) {
                    asked.model = model;
                    asked.subset = choice.subset;
                    asked.material = choice.material;
                } else if (ts2::scene::lower(model) == ts2::scene::lower(asked.model)) {
                    asked.also.emplace_back(choice.material, choice.subset);
                }
            }
            if (!asked.model.empty()) {
                previewToDraw_ = std::move(asked);
                previewGrid_ = &grid;
            }
        }
    }
    // Colour pictures drawn since the description was shown take the place of the empty ones.
    if (coloursDrawn_) {
        if (const auto shown = described_.find(&grid); shown != described_.end()) {
            grid.showStripPictures(colourStrips(life, shown->second, &grid == boughtFrom_));
        }
    }
    // A drawn picture of the item described takes the place of the one it opened with.
    if (const auto shown = described_.find(&grid); shown != described_.end() && drawnNow_.contains(shown->second) && !fireplaces_.contains(shown->second)) {
        grid.showDescriptionPicture(objectPicture(life, shown->second, kDescriptionPictureSide));
    }
    for (const std::uint32_t guid : drawnNow_) {
        grid.pictureChanged(guid);
    }
    const auto start = std::chrono::steady_clock::now();
    const auto read = [&](std::uint32_t guid) { return objectPictures_.contains({guid, kPictureSide << 16 | kPictureSide, 0}); };
    grid.showPictures([&](std::uint32_t guid) -> const ts2::tga::Pixels* {
        if (const auto fireplace = fireplaces_.find(guid); fireplace != fireplaces_.end() && fireplace->second.picture) {
            return &*fireplace->second.picture;
        }
        if (!read(guid) && std::chrono::steady_clock::now() - start > kShownBudget) {
            return nullptr;
        }
        return objectPicture(life, guid, kPictureSide);
    });
    const auto ahead = std::chrono::steady_clock::now();
    for (const std::uint32_t guid : grid.items()) {
        if (std::chrono::steady_clock::now() - ahead > kAheadBudget) {
            break;
        }
        if (!read(guid)) {
            objectPicture(life, guid, kPictureSide);
        }
    }
    // An item is affordable when its price is nothing or within the household's funds, the test the
    // hand makes as it puts a purchase down.
    grid.showAffordable([&](std::uint32_t guid) { return life.handAffords(guid); });
}

const ts2::tga::Pixels* LotUi::colourPicture(std::uint32_t group, const std::string& model, const std::string& subset, const std::string& material,
                                             std::uint32_t width, std::uint32_t height) {
    constexpr std::uint32_t kMaxPictureSide = 128;
    if (!colourThumbnails_) {
        colourThumbnails_.emplace(objectThumbnailsFile_.empty() ? objectThumbnailsFile_ : objectThumbnailsFile_.parent_path() / "DesignModeThumbnails.package");
    }
    const std::uint32_t key = ts2::content::ObjectThumbnails::colourKey(group, model, subset, material);
    auto it = colourPictures_.find({group, key, width << 16 | height});
    if (it == colourPictures_.end()) {
        std::optional<ts2::tga::Pixels> made;
        if (const auto image = colourThumbnails_->pictureAt(group, key, kMaxPictureSide)) {
            made = fitted(*image, width, height);
        } else if (const auto drawn = drawnColours_.find({group, key}); drawn != drawnColours_.end()) {
            // The game draws the picture of a colour it hasn't cached.
            if (drawn->second) {
                made = fitted(*drawn->second, width, height);
            }
        } else {
            coloursToDraw_.try_emplace({group, key}, PictureToDraw{0, model, subset, material, group, key});
        }
        it = colourPictures_.emplace(std::tuple{group, key, width << 16 | height}, std::move(made)).first;
    }
    return it->second ? &*it->second : nullptr;
}

std::vector<SwatchStrip::Choices> LotUi::designSwatches(LotLife& life) {
    constexpr std::uint32_t kSwatchSide = 44;
    const auto lower = [](std::string m) {
        std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (m.ends_with("_cres")) {
            m.resize(m.size() - 5);
        }
        return m;
    };
    // Each model's object group, which files its pictures. A choice filed under another model (a
    // subset's materials mesh) takes the object's own.
    std::map<std::string, std::uint32_t> groups;
    std::uint32_t own = 0;
    for (const auto& [model, group] : life.designModels(designObject_)) {
        groups.emplace(lower(model), group);
        own = own != 0 ? own : group;
    }
    // A swatch shows the object's own model in the colour, also when the colour is filed under another.
    std::map<std::string, std::string> drawn;  // by lowercase subset
    for (const auto& d : life.designSubsets(designObject_)) {
        drawn.emplace(lower(d.subset), d.model);
    }
    std::vector<SwatchStrip::Choices> strips;
    for (const auto& strip : designStrips_) {
        SwatchStrip::Choices shown{{}, {}, strip.chosen};
        for (const auto& choice : strip.choices) {
            const auto mine = drawn.find(lower(choice.subset));
            const std::string& model = mine != drawn.end() ? mine->second : choice.model;
            const auto owner = groups.find(lower(model));
            const std::uint32_t group = owner != groups.end() ? owner->second : own;
            shown.pictures.push_back(colourPicture(group, model, choice.subset, choice.material, kSwatchSide, kSwatchSide));
        }
        strips.push_back(std::move(shown));
    }
    return strips;
}

bool LotUi::openDesign(LotLife& life, std::uint32_t object, int x, int y, std::int32_t cost) {
    constexpr std::uint32_t kCostLine = 0x00930015;  // string id, leakcheck: ok
    const auto sale = life.handSale(object);
    designStrips_ = life.designStrips(object);
    if (!sale || designStrips_.empty()) {
        return false;
    }
    designObject_ = object;
    const auto strips = designSwatches(life);
    std::string line = screen_.data().text(kUiText, kCostLine);
    if (const auto at = line.find("%s"); at != std::string::npos) {
        line.replace(at, 2, fundsText(cost));
    }
    return designDialog_.open(screen_, x, y, life.objectName(sale->guid), line, strips);
}

std::vector<ts2::content::RecolourChoice> LotUi::designedColours(bool replaced) const {
    std::vector<ts2::content::RecolourChoice> out;
    const auto chosen = designDialog_.chosen();
    for (std::size_t k = 0; k < designStrips_.size() && k < chosen.size(); ++k) {
        const auto& strip = designStrips_[k];
        if (chosen[k] != strip.chosen && chosen[k] < strip.choices.size() && strip.chosen < strip.choices.size()) {
            out.push_back(strip.choices[replaced ? strip.chosen : chosen[k]]);
        }
    }
    return out;
}

std::optional<LotUi::FloorChoice> LotUi::floorChoice() const {
    const auto it = std::find_if(coverings_.begin(), coverings_.end(), [&](const auto& c) {
        return c.guid == floorChosen_ && c.kind == ts2::content::CoveringKind::Floor;
    });
    if (floorChosen_ == 0 || it == coverings_.end()) {
        return std::nullopt;
    }
    return FloorChoice{ts2::content::coveringPictureName(it->guid), it->cost};
}

std::optional<LotUi::FloorChoice> LotUi::wallCoveringChoice() const {
    const auto it = std::find_if(coverings_.begin(), coverings_.end(), [&](const auto& c) {
        return c.guid == wallCoveringChosen_ && c.kind == ts2::content::CoveringKind::Wall;
    });
    if (wallCoveringChosen_ == 0 || it == coverings_.end()) {
        return std::nullopt;
    }
    return FloorChoice{ts2::content::coveringPictureName(it->guid), it->cost};
}

std::optional<int> LotUi::wallCoveringPrice(const std::string& pattern) const {
    const auto* found = covering(pattern, ts2::content::CoveringKind::Wall);
    return found != nullptr ? std::optional<int>(found->cost) : std::nullopt;
}

std::optional<int> LotUi::floorPrice(const std::string& pattern) const {
    const auto* found = covering(pattern, ts2::content::CoveringKind::Floor);
    return found != nullptr ? std::optional<int>(found->cost) : std::nullopt;
}

void LotUi::eyedropCovering(const ts2::content::Covering& covering) {
    if (mode_ != Mode::Build) {
        setMode(Mode::Build);
    }
    const bool wall = covering.kind == ts2::content::CoveringKind::Wall;
    buildPanel_.showCovering(covering.guid, wall);
    (wall ? wallCoveringChosen_ : floorChosen_) = covering.guid;
    (wall ? wallCoveringPicked_ : floorPicked_) = true;
}

void LotUi::eyedropItem(std::uint32_t guid, bool build) {
    // Fireplaces have no build type but are listed in Build mode.
    const Mode mode = build || fireplaces_.contains(guid) ? Mode::Build : Mode::Buy;
    if (mode_ != mode) {
        setMode(mode);
    }
    eyedropShown_ = guid;
}

const ts2::content::Covering* LotUi::covering(const std::string& pattern, ts2::content::CoveringKind kind) const {
    std::string name = pattern;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool wall = kind == ts2::content::CoveringKind::Wall;
    // A name of up to 8 hex digits is the covering's GUID, leading zeros or not.
    if (!name.empty() && name.size() <= 8 && name.find_first_not_of("0123456789abcdef") == std::string::npos) {
        if (const auto it = coveringByGuid_.find({wall, static_cast<std::uint32_t>(std::stoul(name, nullptr, 16))}); it != coveringByGuid_.end()) {
            return &coverings_[it->second];
        }
    }
    for (const std::string& file : {name, std::string(wall ? "wall_" : "floor_") + name}) {
        if (const auto it = coveringByFile_.find({wall, file}); it != coveringByFile_.end()) {
            return &coverings_[it->second];
        }
    }
    return nullptr;
}

std::vector<ts2::content::RecolourChoice> LotUi::boughtColours() {
    std::vector<ts2::content::RecolourChoice> out;
    if (boughtFrom_ == nullptr) {
        return out;
    }
    const auto chosen = boughtFrom_->chosenColours();
    for (std::size_t k = 0; k < buyStrips_.size() && k < chosen.size(); ++k) {
        if (chosen[k] != buyStrips_[k].chosen && chosen[k] < buyStrips_[k].choices.size()) {
            out.push_back(buyStrips_[k].choices[chosen[k]]);
        }
    }
    return out;
}

std::vector<CatalogGrid::Strip> LotUi::colourStrips(LotLife& life, std::uint32_t guid, bool forBuying) {
    constexpr std::uint16_t kModelNames = 0x85;
    constexpr std::size_t kModelIndexLow = 0x48, kModelIndexHigh = 0x49;
    constexpr std::uint32_t kSwatchSide = 44, kDescriptionPictureSide = 117;
    auto& code = life.simulation().code();
    const auto* type = code.objectType(guid);
    if (type == nullptr) {
        return {};
    }
    // A multi-tile object's colours are its parts' models'.
    std::vector<const ts2::sim::ObjectType*> types{type};
    if (type->definition.isMultiTileMaster()) {
        types = code.multiTileParts(*type);
    }
    // A model name as the recolours and the models' string tables may each write it.
    const auto bare = [](std::string m) {
        std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (m.ends_with("_cres")) {
            m.resize(m.size() - 5);
        }
        return m;
    };
    std::vector<std::string> models;
    std::map<std::string, std::uint32_t> groups;  // each model's object group, which files its pictures
    for (const auto* t : types) {
        const auto& d = t->definition;
        const std::uint32_t index = d.field(kModelIndexLow) | static_cast<std::uint32_t>(d.field(kModelIndexHigh)) << 16;
        if (auto name = code.string(t->group, kModelNames, index); name && !name->empty()) {
            groups.emplace(bare(*name), t->group);
            models.push_back(std::move(*name));
        }
    }
    std::vector<CatalogGrid::Strip> strips;
    const auto subsets = life.designSubsetsOf(models, guid);
    std::map<std::string, std::string> drawn;  // each subset's own model, which its swatches show
    for (const auto& d : subsets) {
        drawn.emplace(bare(d.subset), d.model);
    }
    auto found = ts2::content::recolourStrips(life.simulation().recolours(), subsets);
    shownStrips_[guid] = {found, drawn};
    if (forBuying) {
        buyStrips_ = found;
    }
    for (const auto& strip : found) {
        CatalogGrid::Strip shown{{}, {}, strip.chosen};
        for (const auto& choice : strip.choices) {
            const auto mine = drawn.find(bare(choice.subset));
            const std::string& model = mine != drawn.end() ? mine->second : choice.model;
            const auto owner = groups.find(bare(model));
            const std::uint32_t group = owner != groups.end() ? owner->second : type->group;
            shown.pictures.push_back(colourPicture(group, model, choice.subset, choice.material, kSwatchSide, kSwatchSide));
            shown.large.push_back(colourPicture(group, model, choice.subset, choice.material, kDescriptionPictureSide, kDescriptionPictureSide));
        }
        strips.push_back(std::move(shown));
    }
    return strips;
}

// While the hand holds an object, the held object's panel takes the place of the Buy or Build
// panel. Its Buy or Build half shows the object's picture, and its catalog name over the line telling
// what it sells for (UI text 0x88 line 5). Not done yet: the names of objects with a name of their own.
void LotUi::showHeld(LotLife& life) {
    constexpr std::uint32_t kBuyHalf = 0xA, kBuildHalf = 0xB, kText = 1, kPicture = 3;
    constexpr std::uint32_t kSellLine = 0x00880005;  // string id, leakcheck: ok
    if (held_ < 0 || loaded_ < 2) {
        return;
    }
    const bool shown = heldObject_ != 0 && (mode_ == Mode::Buy || mode_ == Mode::Build) && !hidden_;
    const bool build = mode_ == Mode::Build;
    const auto showPanel = [&](int panel, bool show) {
        if (panel >= 0) {
            screen_.setVisible(panel, show);
        }
    };
    if (shown) {
        showPanel(buy_, false);
        showPanel(build_, false);
    } else if (screen_.visible(held_)) {
        showPanel(buy_, mode_ == Mode::Buy && !hidden_);
        showPanel(build_, build && !hidden_);
        heldFilled_ = 0;
    }
    screen_.setVisible(held_, shown);
    if (!shown || (heldFilled_ == heldObject_ && heldInBuild_ == build)) {
        return;
    }
    const auto sale = life.handSale(heldObject_);
    if (!sale) {
        return;
    }
    heldFilled_ = heldObject_;
    heldInBuild_ = build;
    const int buyHalf = child(screen_, held_, kBuyHalf), buildHalf = child(screen_, held_, kBuildHalf);
    screen_.setVisible(buyHalf, !build);
    screen_.setVisible(buildHalf, build);
    const int half = build ? buildHalf : buyHalf;
    std::string line = screen_.data().text(kUiText, kSellLine);
    if (const auto at = line.find("%s"); at != std::string::npos) {
        line.replace(at, 2, fundsText(sale->value));
    }
    // The line breaks before "(press ESC to cancel)" with a carriage return alone.
    for (std::size_t at = line.find('\r'); at != std::string::npos; at = line.find('\r', at)) {
        if (at + 1 < line.size() && line[at + 1] == '\n') {
            line.erase(at, 1);
        } else {
            line[at] = '\n';
        }
    }
    // A diagonal door or window isn't in the catalog and shows its straight version.
    const std::uint32_t shownItem = life.eyedropGuid(heldObject_).value_or(sale->guid);
    screen_.setText(child(screen_, half, kText), life.objectName(shownItem) +"\n\n" + line);
    const int picture = child(screen_, half, kPicture);
    if (picture >= 0) {
        const auto w = static_cast<std::uint32_t>(screen_.width(picture)), h = static_cast<std::uint32_t>(screen_.height(picture));
        // A recoloured object shows its colour's picture.
        const ts2::tga::Pixels* pixels = nullptr;
        if (const auto colour = shownItem == sale->guid ? life.handRecolourOf(heldObject_) : std::nullopt) {
            pixels = colourPicture(colour->second, colour->first.model, colour->first.subset, colour->first.material, w, h);
        }
        if (pixels == nullptr) {
            pixels = objectPicture(life, shownItem, w, h);
        }
        screen_.setImage(picture, pixels != nullptr ? *pixels : ts2::tga::Pixels{});
    }
}

const LotUi::SharedButtons* LotUi::sharedButtons() const {
    static constexpr SharedButtons kBuy{0xCC607031, 0xCC607030, 0xCC607032, 0xAE, 0xCC607035, 0xCC607036, 0xCC6070AF, 0xCC607033, 0xCC607034};
    static constexpr SharedButtons kBuild{0xA5, 0xAB, 0xA4, 0xAE, 0xAA, 0xA3, 0xAF, 0xA1, 0xA2};
    return mode_ == Mode::Buy ? &kBuy : mode_ == Mode::Build ? &kBuild : nullptr;
}

// The shown panel's tool buttons: the tool in use shows pressed (the hand only while it holds no
// catalog item), and the undo and redo arrows can be pressed while the history has a change for them.
void LotUi::showBuyTools() {
    const SharedButtons* b = sharedButtons();
    const int panel = mode_ == Mode::Buy ? buy_ : build_;
    if (b == nullptr || panel < 0) {
        return;
    }
    screen_.setChecked(screen_.find(panel, b->hand), !buying_ && tool_ == Tool::Hand);
    screen_.setChecked(screen_.find(panel, b->design), tool_ == Tool::Design);
    screen_.setChecked(screen_.find(panel, b->eyedropper), tool_ == Tool::Eyedropper);
    screen_.setChecked(screen_.find(panel, b->sledgehammer), tool_ == Tool::Sledgehammer);
    screen_.setChecked(screen_.find(panel, b->topDown), topDownView_);
    screen_.setChecked(screen_.find(panel, b->ceiling), ceilingView_);
    screen_.setEnabled(screen_.find(panel, b->undo), canUndo_);
    screen_.setEnabled(screen_.find(panel, b->redo), canRedo_);
    if (mode_ == Mode::Build) {
        buildPanel_.setWallTool(tool_ == Tool::Wall);
    }
}

RewardsPanel::Sim LotUi::rewardsOf(LotLife& life, std::int16_t sim) {
    const auto data = life.personData(sim);
    const auto value = [&](std::size_t index) -> std::int16_t { return index < data.size() ? data[index] : 0; };
    RewardsPanel::Sim rewards;
    rewards.aspiration = life.aspiration();
    if (const auto token = life.inventoryValue(sim, kAspirationToken); token && *token > 0) {
        rewards.aspiration = static_cast<std::int16_t>(rewards.aspiration ^ *token);
    }
    rewards.careerTab = value(kAge) > 15 && value(kYoungAdult) == 0;
    rewards.aspirationTab = value(kAge) >= 2;
    for (const auto& object : life.inventoryObjects(sim)) {
        rewards.inventory.push_back({objectPicture(life, object.guid, kSmallRewardPicture), object.count, true, 0});
    }

    // The reward objects are the catalog's with OBJD field 0x28 bit 0x400 (aspiration) or 0x800
    // (career), cheapest first. Parts of larger objects aren't in the catalog.
    auto& code = life.simulation().code();
    if (!rewardObjects_) {
        rewardObjects_.emplace();
        std::vector<const ts2::sim::ObjectType*> aspiration, career;
        for (const std::uint32_t guid : code.objectTypeGuids()) {
            const auto* type = code.objectType(guid);
            if (type == nullptr || (type->definition.field(ts2::objd::field::kMultiTileMasterId) != 0 &&
                                    type->definition.field(ts2::objd::field::kMultiTileSubIndex) != 0xFFFF)) {
                continue;
            }
            const std::uint16_t kind = type->definition.field(kRewardKind);
            if (kind & kAspirationReward) {
                aspiration.push_back(type);
            } else if (kind & kCareerReward) {
                career.push_back(type);
            }
        }
        for (auto [types, out] : {std::pair{&aspiration, &rewardObjects_->first}, std::pair{&career, &rewardObjects_->second}}) {
            std::stable_sort(types->begin(), types->end(), [](const auto* a, const auto* b) {
                return a->definition.field(ts2::objd::field::kPrice) < b->definition.field(ts2::objd::field::kPrice);
            });
            for (const auto* type : *types) {
                out->push_back(type->definition.guid());
            }
        }
    }
    // Aspiration rewards cost points (person data 0xA1 times 10). Career rewards are the ones the
    // Sim's career has given it.
    rewards.points = std::max(0, value(kRewardPoints) * 10);
    for (const std::uint32_t guid : rewardObjects_->first) {
        const auto* type = code.objectType(guid);
        const int price = type ? type->definition.field(ts2::objd::field::kPrice) : 0;
        rewards.aspirationRewards.push_back({objectPicture(life, guid, kSmallRewardPicture), 1, price <= rewards.points, price});
    }
    for (const std::uint32_t guid : rewardObjects_->second) {
        rewards.careerRewards.push_back(
            {objectPicture(life, guid, kLargeRewardWidth, kLargeRewardHeight), 1, life.hasCountedToken(sim, guid), 0, careerIcon(life, guid)});
    }
    // The lifetime aspiration tree comes with Free Time. It has levels to spend when person data
    // 0xD9 is above 0xDA.
    rewards.lifetimeTree = (life.simulation().installedPacks() & kFreeTime) != 0;
    rewards.lifetimeLevels = value(kLifetimeLevels) > value(kLifetimeLevelsSpent);
    return rewards;
}

std::vector<MemoriesPage::Memory> LotUi::panelMemories(LotLife& life, std::int16_t sim) {
    // A memory's picture is image 1 of its object's group, as the game's thumbnail service takes it.
    constexpr std::uint32_t kPicture = 1, kMaxPictureSide = 256;
    std::vector<MemoriesPage::Memory> out;
    for (const auto& m : life.memories(sim)) {
        auto picture = memoryPictures_.find(m.pictureGroup);
        if (picture == memoryPictures_.end()) {
            picture = memoryPictures_.emplace(m.pictureGroup, life.simulation().code().image(m.pictureGroup, kPicture, kMaxPictureSide)).first;
        }
        MemoriesPage::Memory shown;
        shown.name = m.name;
        shown.description = m.description;
        shown.subjectName = m.subjectFullName;
        shown.good = m.good;
        shown.picture = picture->second ? &*picture->second : nullptr;
        // A Sim's portrait, or the picture of the object (career, ...) the memory is about.
        shown.subjectPicture = m.subjectNeighbor != 0 ? targetPicture(memoryPortraits_, life, m.subjectNeighbor)
                               : m.subject != 0       ? objectPicture(life, m.subject, kMemoryPortraitSize)
                                                      : nullptr;
        out.push_back(std::move(shown));
    }
    return out;
}

std::vector<RelationshipsPanel::Person> LotUi::panelRelationships(LotLife& life, std::int16_t sim) {
    std::vector<RelationshipsPanel::Person> out;
    for (const auto& r : life.relationships(sim)) {
        RelationshipsPanel::Person p;
        p.neighbor = r.neighbor;
        p.daily = r.daily;
        p.lifetime = r.lifetime;
        p.flags = r.flags;
        p.family = r.familyTie;
        p.household = r.household;
        p.pet = r.species != 0;
        p.female = r.female;
        p.coins = r.coins;
        p.skip = r.skip;
        p.chemistry = chemistryLevel(r);
        // Sims on the lot come first (the Sims of the selected Sim's current and next interactions
        // before them are not done yet), then by lifetime.
        p.sortGroup = r.onLot ? 0 : 1;
        p.picture = targetPicture(relationshipPictures_, life, r.neighbor);
        out.push_back(std::move(p));
    }
    return out;
}

// The Aspiration Meter popup's texts, as the game fills them: the lines with the Sim's first name,
// the level's name by age, aspiration, level and gender with "(Gold)" or "(Platinum)" when the
// meter is that high, and the lifetime want the neighborhood saved for the Sim.
AspirationPopup::Content LotUi::aspirationPopupContent(LotLife& life, std::int16_t sim) {
    constexpr std::uint32_t kLiveText = 0x7F96C284, kPanelText = 0x91, kValues = 0x9F;
    constexpr std::uint32_t kLevelLine = 0x18, kGold = 0x26, kPlatinum = 0x27, kLifetimeLine = 8;
    constexpr std::size_t kLifeStage = 0x3A, kGender = 0x41, kAspiration = 0x2E, kScore = 0xA2;
    auto text = [&](std::uint32_t table, std::uint32_t n) { return screen_.data().text(kLiveText, table << 16 | n); };
    const auto data = life.personData(sim);
    const auto value = [&](std::size_t index) -> std::int16_t { return index < data.size() ? data[index] : 0; };
    AspirationPopup::Content content;
    const std::string first = life.simName(sim).first;
    content.levelLine = ts2::content::formatted(text(kPanelText, kLevelLine), {first});
    content.lifetimeLine = ts2::content::formatted(text(kValues, kLifetimeLine), {first});

    // The level's name: a table by age, a block in it by aspiration (toddlers and children have one
    // block each), then two rows a level, male first.
    std::int16_t aspiration = value(kAspiration);
    if (const auto secondary = life.inventoryValue(sim, kAspirationToken); secondary && *secondary > 0) {
        aspiration = static_cast<std::int16_t>(aspiration ^ *secondary);
    }
    auto blockOf = [](std::int16_t a) -> std::uint32_t {
        switch (a) {
        case 1: return 0x01;     // Romance
        case 2: return 0x0E;     // Family
        case 0x10: return 0x1B;  // Popularity
        case 4: return 0x28;     // Fortune
        case 0x20: return 0x35;  // Knowledge
        case 0x80: return 0x42;  // Pleasure
        case 0x100: return 0x4F; // Grilled Cheese
        default: return 0;
        }
    };
    constexpr std::int16_t kToddler = 2, kChild = 3, kTeen = 16, kAdult = 19, kElder = 51;
    const std::int16_t stage = value(kLifeStage);
    std::uint32_t table = 0, block = 0;
    if (stage == kAdult) {
        table = 0x9A, block = blockOf(aspiration);
    } else if (stage == kTeen) {
        table = 0x9C, block = blockOf(aspiration);
    } else if (stage == kElder) {
        table = 0x9D, block = blockOf(aspiration);
    } else if (stage == kToddler) {
        table = 0x9B, block = 0x01;
    } else if (stage == kChild) {
        table = 0x9B, block = 0x0E;
    }
    const std::array<std::int32_t, 7>* levels = nullptr;
    if (const auto* definitions = life.simulation().world().wantDefinitions()) {
        if (const auto it = definitions->aspirationLevels.find(ts2::sim::wantAgeCode(stage)); it != definitions->aspirationLevels.end()) {
            levels = &it->second;
        }
    }
    const std::int32_t score = value(kScore) * 10;
    // Without its age's levels a Sim counts as normal.
    const std::int32_t band = levels != nullptr ? ts2::sim::aspirationBand(*levels, score) : 2;
    if (table != 0 && block != 0) {
        content.level = text(table, block + 2 * static_cast<std::uint32_t>(band) + (value(kGender) == 1 ? 1 : 0));
    }
    const std::int32_t meter = levels != nullptr ? ts2::sim::aspirationLevel(*levels, score) : 0;
    if (meter >= 75) {
        content.level += " " + text(kPanelText, kPlatinum);
    } else if (meter >= 50) {
        content.level += " " + text(kPanelText, kGold);
    }

    // The lifetime want: its name with its values on the line under it, its blurb and its icon.
    if (const auto* want = life.simulation().world().lifetimeWant(life.neighborId(sim))) {
        if (const auto* d = life.wantDefinition(want->guid)) {
            WantsPanel::Want p;
            wantPopupTexts(life, *want, *d, p);
            AspirationPopup::Lifetime lifetime;
            // The tooltip is the name and a line for each value. Here the values share one line.
            lifetime.name = p.tip;
            if (const auto firstBreak = lifetime.name.find('\n'); firstBreak != std::string::npos) {
                std::replace(lifetime.name.begin() + static_cast<std::ptrdiff_t>(firstBreak) + 1, lifetime.name.end(), '\n', ' ');
            }
            lifetime.blurb = p.description;
            lifetime.icon = d->primaryIcon;
            lifetime.fulfilled = !want->tail.empty() && want->tail[0] != 0;
            content.lifetime = std::move(lifetime);
        }
    }
    return content;
}

std::string LotUi::fullName(LotLife& life, std::int16_t sim) const {
    constexpr std::uint32_t kLiveText = 0x7F96C284, kTitles = 0x336;
    const LotLife::SimName parts = life.simName(sim);
    std::string name = parts.first.empty() ? parts.last : parts.last.empty() ? parts.first : parts.first + " " + parts.last;
    if (parts.title > 0) {
        if (const std::string title = screen_.data().text(kLiveText, kTitles << 16 | static_cast<std::uint32_t>(parts.title)); !title.empty()) {
            name = ts2::content::formatted(title, {name});
        }
    }
    return name;
}

void LotUi::showHousehold(std::vector<Skewer::Member> members) {
    namesShown_.clear();  // the new thumbnails have no names yet
    portraits_.clear();
    targets_.clear();
    relationshipPictures_.clear();
    phoneBookPortraits_.clear();
    memoryPortraits_.clear();
    wantPopupPortraits_.clear();
    balloonPortraits_.clear();
    // A newborn's file holds no portrait, so a picture asked for before the household's were drawn
    // was kept empty.
    queuePortraits_.clear();
    notificationPortraits_.clear();
    notificationSmallPortraits_.clear();
    drawnPortraits_.clear();
    portraitsWanted_.clear();
    panel_.refreshWants();  // their pictures may be of the last household
    wantsPending_ = true;  // the wants shown point at the pictures just cleared
    portraitShown_ = -1;
    for (auto& m : members) {
        // A broken character file could hold a huge picture. Portraits are 256 pixels.
        if (m.portrait && (m.portrait->width > ts2::content::kMaxPortraitSide || m.portrait->height > ts2::content::kMaxPortraitSide)) {
            m.portrait.reset();
        }
        if (m.portrait) {
            portraits_[m.sim] = *m.portrait;
        }
    }
    hasMembers_ = !members.empty();
    skewer_.setHousehold(std::move(members));
    showPetsBackground(skewer_.hasBranch());
    if (!hidden_) {
        setMode(mode_);
    }
}

void LotUi::setDrawnPortrait(std::int16_t neighbor, std::optional<ts2::tga::Pixels> portrait) {
    std::erase(portraitsWanted_, neighbor);
    if (portrait && (portrait->width > ts2::content::kMaxPortraitSide || portrait->height > ts2::content::kMaxPortraitSide)) {
        portrait.reset();
    }
    drawnPortraits_[neighbor] = std::move(portrait);
}

void LotUi::setPortrait(std::int16_t sim, const ts2::tga::Pixels& portrait) {
    if (!portraits_.contains(sim)) {
        return;
    }
    portraits_[sim] = portrait;
    skewer_.setPortrait(sim, portrait);
    queuePortraits_.clear();
    notificationPortraits_.clear();
    notificationSmallPortraits_.clear();
    if (sim == portraitShown_) {
        portraitShown_ = -1;  // the live panel shows it again
    }
}

void LotUi::showLife(LotLife& life) {
    ts2::diag::Timer timer("lot ui");
    show(life.clockText(), life.speed(), life.paused(), life.funds());
    const std::int16_t sim = life.selected();
    skewer_.select(sim);
    buyPanel_.grid().tick();
    buildPanel_.grid().tick();
    showCatalogs(life);
    showHeld(life);
    showBuyTools();
    // A pet has its own live panel.
    constexpr std::size_t kSpecies = 0xBA;
    const auto selectedData = life.personData(sim);
    const bool pet = selectedData.size() > kSpecies && selectedData[kSpecies] != 0;
    if (pet != petShown_ && petLive_ >= 0) {
        const bool shown = screen_.visible(live_) || screen_.visible(petLive_);
        petShown_ = pet;
        showLivePanel(shown);
        portraitShown_ = -1;
    }
    targets_.collect();
    relationshipPictures_.collect();
    memoryPortraits_.collect();
    wantPopupPortraits_.collect();
    queuePortraits_.collect();
    phoneBookPortraits_.collect();
    balloonPortraits_.collect();
    // The live panel shows the selected Sim's portrait over its shadow.
    if (sim != portraitShown_ && live_ >= 0) {
        portraitShown_ = sim;
        const int holder = screen_.find(shownPanel(), kPortraitHolder);
        const auto it = portraits_.find(sim);
        screen_.setVisible(holder, it != portraits_.end());
        if (it != portraits_.end()) {
            // The portrait is scaled to the window's picture and cut to its shape.
            const auto& w = portraitWindows_;
            screen_.setImage(screen_.find(holder, kPanelPortrait),
                             w.shape.width > 0 ? ts2::content::masked(ts2::content::scaled(it->second, w.shape.width, w.shape.height), w.shape)
                                               : it->second);
            // The shadow is the portrait's silhouette moved down and right, then scaled and cut the same way.
            const int shadow = screen_.find(holder, kPanelShadow);
            const auto silhouette = ts2::content::portraitSilhouette(it->second);
            const bool shadowShown = w.shadowShape.width > 0 && silhouette.width > 0;
            if (shadowShown) {
                const auto moved = ts2::content::placed(silhouette, silhouette.width, silhouette.height, w.shadowX, w.shadowY);
                screen_.setImage(shadow, ts2::content::masked(ts2::content::scaled(moved, w.shadowShape.width, w.shadowShape.height), w.shadowShape));
            }
            screen_.setVisible(shadow, shadowShown);
        }
        starShown_.reset();
        showTracked(tracked_);
    }
    // The Aspiration Meter popup moves on under a pet's panel too, which hides it.
    panel_.updateAspirationPopup([&] { return aspirationPopupContent(life, sim); });
    if (petShown_) {
        showPetPanel(life, sim);
    } else {
        showSimPanel(life, sim);
    }
    constexpr std::size_t kUiIconFlags = 0x34, kRoommateFlags = 0x54;
    for (const std::int16_t member : life.household()) {
        skewer_.setMood(member, life.mood(member));
        const auto data = life.personData(member);
        const auto memberPersonData = [&](std::size_t index) {
            return index < data.size() ? static_cast<std::uint16_t>(data[index]) : std::uint16_t{0};
        };
        std::uint16_t awayFlags = memberPersonData(kUiIconFlags);
        // A pet with person data 0x54 bit 0x80 shows as run away. Its reinforce behavior icon
        // comes from training data not known yet.
        if (memberPersonData(kSpecies) != 0 && (memberPersonData(kRoommateFlags) & 0x80) != 0) {
            awayFlags |= 4;
        }
        skewer_.setAwayIcons(member, awayFlags, (memberPersonData(kRoommateFlags) & 0x800) != 0);
        const auto* who = life.simulation().world().find(member);
        skewer_.setPressable(member, who == nullptr || !ts2::sim::passedOver(*who));
    }
    showNames(life, sim);
    showQueue(life, sim);
}

// The pet panel's needs, mood, age and relationships for the selected pet.
void LotUi::showPetPanel(LotLife& life, std::int16_t sim) {
    constexpr std::size_t kSpecies = 0xBA;
    const auto personData = life.personData(sim);
    const auto person = [&](std::size_t index) -> std::int16_t { return index < personData.size() ? personData[index] : 0; };
    const std::int16_t species = person(kSpecies);
    petPanel_.setNeeds(sim, life.lifeStage(sim), species, life.motives(sim));
    petPanel_.setMood(life.mood(sim));
    const auto age = life.ageProgress(sim);
    petPanel_.setAge(species, life.lifeStage(sim), age.lived, age.length,
                     ts2::content::petAgeTip(species, life.lifeStage(sim), person(kAgeDuration), person(kAgeDaysLeft), person(kDaysBeforeElder)));
    if (petPanel_.showsRelationships()) {
        petPanel_.setRelationships(panelRelationships(life, sim), species);
    }
    if (petPanel_.sub() == PetPanel::Sub::Job) {
        // A pet has a job when person data 0x39 is set and its career (GUID person data 0x5A, 0x59)
        // exists.
        constexpr std::size_t kJob = 0x39, kCareerLow = 0x59, kCareerHigh = 0x5A;
        const auto guid = static_cast<std::uint32_t>(static_cast<std::uint16_t>(person(kCareerHigh))) << 16 |
                          static_cast<std::uint16_t>(person(kCareerLow));
        petPanel_.setJob(personData, person(kJob) != 0 && life.simulation().code().objectType(guid) != nullptr);
    } else if (petPanel_.sub() == PetPanel::Sub::Petology) {
        petPanel_.setPetology(personData);
    }
}

// The Sims' live panel for the selected Sim: aspiration, needs, mood, age, the open sub-panel and
// the wants.
void LotUi::showSimPanel(LotLife& life, std::int16_t sim) {
    panel_.setAspiration(life.aspiration());
    if (panel_.sub() == LivePanel::Sub::Rewards) {
        panel_.setRewards(rewardsOf(life, sim));
    }
    panel_.setNeeds(sim, life.lifeStage(sim), life.motives(sim));
    panel_.setMood(life.mood(sim), life.aspirationScore(sim));
    const auto personData = life.personData(sim);
    const auto person = [&](std::size_t index) -> std::int16_t { return index < personData.size() ? personData[index] : 0; };
    panel_.setAspirationBars(life.aspirationScore(sim), person(kLifetimeHappiness),
                             ts2::content::influenceMeterValue(life.influenceLevels(), person(kInfluencePoints)),
                             std::clamp<int>(person(kInfluenceLevel), 0, 4));
    const auto age = life.ageProgress(sim);
    const auto tip = ts2::content::ageTip(life.lifeStage(sim), (person(kPersonFlags) & kPlantSim) != 0, person(kAgeDuration), person(kAgeDaysLeft),
                                          person(kDaysBeforeElder));
    panel_.setAge(life.lifeStage(sim), age.lived, age.length, tip);
    // The Secret Lot token is only read while Simology is open, as it is copied out of the inventory.
    const auto secretLots = panel_.sub() == LivePanel::Sub::Simology ? life.inventoryValues(sim, kSecretLotToken) : std::vector<std::int16_t>{};
    panel_.setSimology(life.lifeStage(sim), life.personData(sim), secretLots);
    panel_.setJob(life.personData(sim), life.simulation().globals(), life.simulation().code());
    if (panel_.sub() == LivePanel::Sub::Job) {
        int badges = 0;
        for (const std::uint32_t token : kBadgeTokens) {
            // The game reads the first token of the badge, value 0 counting as none.
            const auto values = life.inventoryValues(sim, token);
            const std::int16_t value = values.empty() ? 0 : values.front();
            badges += value < 0 || value >= kBronzeBadge;
        }
        panel_.setBadges(life.personData(sim), badges);
    }
    if (panel_.showsMemories()) {
        panel_.setMemories(panelMemories(life, sim));
    }
    if (panel_.showsRelationships()) {
        const auto data = life.personData(sim);
        panel_.setRelationships(panelRelationships(life, sim), data.size() > 0xBA ? data[0xBA] : 0);
    }
    // The wants panel's texts and pictures are made again only when the wants change, a name may have
    // changed or a target's portrait is still loading.
    const auto& wants = life.wants(sim);
    const auto& fears = life.fears(sim);
    if (wantsPending_ || sim != wantsSim_ || life.namesVersion() != wantsNames_ || !sameWants(wants, wantsSaved_) ||
        !sameWants(fears, fearsSaved_)) {
        wantsShown_ = panelWants(life, wants);
        fearsShown_ = panelWants(life, fears);
        wantsSaved_ = wants;
        fearsSaved_ = fears;
        wantsSim_ = sim;
        wantsNames_ = life.namesVersion();
        const auto loading = [](const WantsPanel::Want& w) { return w.hasTarget && (!w.targetPicture || !w.popupTargetPicture); };
        wantsPending_ = std::any_of(wantsShown_.begin(), wantsShown_.end(), loading) || std::any_of(fearsShown_.begin(), fearsShown_.end(), loading);
    }
    const auto slots = life.wantSlots(sim);
    panel_.setWants(wantsShown_, fearsShown_, slots.wants, slots.fears, {sim, life.wantsRefilling(sim), &life.satisfiedWantPlaces(sim)});
}

// The household's names as tips over their thumbnails, and the selected Sim's over the live panel's
// portrait, whose script marks it to show its caption as a tip. They are made again only when a name
// may have changed: a first name given, another Sim selected or a witch's title.
void LotUi::showNames(LotLife& life, std::int16_t sim) {
    constexpr std::size_t kTitle = 0xE6;
    const auto title = [&](std::int16_t s) {
        const auto data = life.personData(s);
        return kTitle < data.size() ? data[kTitle] : std::int16_t{0};
    };
    namesScratch_.assign({static_cast<std::int32_t>(life.namesVersion()), sim, title(sim)});
    for (const std::int16_t member : life.household()) {
        namesScratch_.push_back(member);
        namesScratch_.push_back(title(member));
    }
    if (namesScratch_ == namesShown_) {
        return;
    }
    namesShown_.swap(namesScratch_);
    for (const std::int16_t member : life.household()) {
        skewer_.setName(member, fullName(life, member));
    }
    if (live_ >= 0) {
        screen_.setTip(screen_.find(shownPanel(), kPortraitHolder), sim != 0 ? fullName(life, sim) : std::string());
    }
}

// The queue's icons: an object type's picture, or a Sim's portrait.
void LotUi::showQueue(LotLife& life, std::int16_t sim) {
    const std::uint32_t size = queue_.iconSize();
    std::vector<QueuePanel::Item> items;
    for (const auto& action : life.queue(sim)) {
        QueuePanel::Item item{action.id, nullptr, action.running, action.cancelled, action.nested, action.name};
        if (action.neighbor != 0) {
            item.picture = targetPicture(queuePortraits_, life, action.neighbor);
        } else if (action.guid != 0 && size > 0) {
            item.picture = objectPicture(life, action.guid, size, 0, action.index);
        }
        items.push_back(item);
    }
    queue_.show(items);
}

void LotUi::mouseButton(int x, int y, bool down, LotLife& life) {
    buyPanel_.grid().mouseButton(x, y, down);
    buildPanel_.grid().mouseButton(x, y, down);
    const auto at = queue_.itemAt(x, y);
    if (down) {
        queuePressed_ = at;
        return;
    }
    if (at && at == queuePressed_ && life.cancelInteraction(life.selected(), *at)) {
        showQueue(life, life.selected());
    }
    queuePressed_.reset();
}

} // namespace app
