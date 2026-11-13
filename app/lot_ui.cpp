#include "lot_ui.hpp"

#include "content/ui_meters.hpp"
#include "content/ui_queue.hpp"
#include "content/ui_text.hpp"
#include "content/wants.hpp"
#include "diag/timing.hpp"
#include "diagnostics.hpp"
#include "formats/hash.hpp"
#include "names.hpp"

#include <algorithm>
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
// The puck's windows.
constexpr std::uint32_t kLiveMode = 0x37D97B25, kOptions = 0x67D97B2F, kFunds = 0x00003000;
// Buy mode: the puck's Buy button and the Buy panel's script.
constexpr std::uint32_t kBuyMode = 0x67D97B2A, kBuyPanel = 0x49060004;
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
constexpr std::uint32_t kNeighborhood = 0x0000A210, kExitTutorial = 0x0000A211, kQuit = 0x0000A220;

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
        options_ = screen_.load(kScripts, kOptionsMenu, 0, 0);
        buy_ = screen_.load(kScripts, kBuyPanel, 0, 0);
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
        anchor(options_);
        anchor(buy_);
        buyPanel_.attach(screen_, buy_);
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
        portraitWindows_ = portraitWindows(screen_);
        skewer_.attach(screen_, puck_);
        pauseFrame_.attach(screen_, width_, height_);
        queue_.attach(screen_);
        screen_.setVisible(screen_.find(live_, kPortraitHolder), false);
        screen_.setVisible(screen_.find(live_, kVacationDays), false);
        // A household without pets shows the plain background.
        screen_.setVisible(screen_.find(live_, kPetsBackground), false);
        screen_.setVisible(screen_.find(live_, kNoPetsBackground), true);
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
    for (const int panel : {puck_, live_, options_, buy_}) {
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
    for (const int panel : {puck_, live_, options_, buy_, version_}) {
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
        optionsWereShown_ = optionsShown();
        buyWasShown_ = buyShown();
        closeWallMenu();
        queue_.setVisible(false);
        for (const int panel : {puck_, live_, options_, buy_, version_}) {
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
    if (live_ >= 0) {
        screen_.setVisible(live_, true);
    }
    showOptions(optionsWereShown_);
    if (buyWasShown_) {
        showBuy(true);
    }
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

void LotUi::showOptions(bool show) {
    // The options menu is not live mode, so the queue goes with the live panel. It takes Buy mode's place.
    if (show && buy_ >= 0) {
        screen_.setVisible(buy_, false);
        screen_.setChecked(screen_.find(puck_, kBuyMode), false);
    }
    queue_.setVisible(!show);
    if (options_ < 0) {
        return;
    }
    screen_.setVisible(options_, show);
    if (!show) {
        optionsMenu_.close();
    }
    showHoodPanel(show && puck_ >= 0 && screen_.visible(puck_));
    // With nobody in the household, Live can't be used: its button is disabled and the live panel
    // stays hidden.
    screen_.setVisible(live_, !show && hasMembers_);
    skewer_.setLiveMode(!show);
    screen_.setChecked(screen_.find(puck_, kOptions), show);
    const int live = screen_.find(puck_, kLiveMode);
    screen_.setEnabled(live, hasMembers_);
    screen_.setChecked(live, !show && hasMembers_);
}

bool LotUi::optionsShown() const { return options_ >= 0 && screen_.visible(options_); }

void LotUi::showBuy(bool show) {
    if (buy_ < 0) {
        return;
    }
    if (show) {
        if (optionsShown()) {
            showOptions(false);
        }
        buyPanel_.reset();
    }
    screen_.setVisible(buy_, show);
    // Buy mode isn't live mode: the live panel and the queue give way, as they do to the options.
    queue_.setVisible(!show);
    screen_.setVisible(live_, !show && hasMembers_);
    skewer_.setLiveMode(!show);
    screen_.setChecked(screen_.find(puck_, kBuyMode), show);
    const int live = screen_.find(puck_, kLiveMode);
    screen_.setEnabled(live, hasMembers_);
    screen_.setChecked(live, !show && hasMembers_);
    screen_.raise(puck_);
}

bool LotUi::buyShown() const { return buy_ >= 0 && screen_.visible(buy_); }

void LotUi::refreshBuy(LotLife& life) {
    if (!buyShown()) {
        return;
    }
    BuyPanel::Source source;
    source.items = &life.buyItems();
    source.picture = [&](const ts2::sim::catalog::Item& item) { return objectPicture(life, item.guid, kSmallRewardPicture); };
    source.name = [&](const ts2::sim::catalog::Item& item) { return life.catalogText(item, 0); };
    buyPanel_.refresh(source);
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
    const std::uint32_t id = screen_.id(window);
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
        showOptions(!optionsShown());
        return optionsShown() ? Action::OptionsOpened : Action::OptionsClosed;
    }
    // Buy mode: its puck button opens it (with nobody in the household too), Live closes it, and a
    // catalog item clicked is bought.
    if (id == kBuyMode && screen_.parent(window) == puck_) {
        if (buyShown()) {
            return Action::None;
        }
        buyFromOptions_ = optionsShown();
        showBuy(true);
        return Action::BuyOpened;
    }
    if (id == kLiveMode && screen_.parent(window) == puck_ && buyShown()) {
        showBuy(false);
        return Action::BuyClosed;
    }
    if (const auto click = buyPanel_.clicked(window); click.panel) {
        if (click.buy) {
            pickedBuyItem_ = *click.buy;
            return Action::BuyItem;
        }
        return Action::None;
    }
    if (graphicsOptions_.clicked(window)) {
        return graphicsOptions_.takeChanged() ? Action::GraphicsOptionChanged : Action::None;
    }
    if (gameOptions_.clicked(window)) {
        return gameOptions_.takeChanged() ? Action::GameOptionChanged : Action::None;
    }
    if (id == kLiveMode && screen_.parent(window) == puck_ && optionsShown()) {
        showOptions(false);
        return Action::OptionsClosed;
    }
    const bool rewardsWereOpen = panel_.sub() == LivePanel::Sub::Rewards;
    if (panel_.clicked(window)) {
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
        screen_.setVisible(screen_.find(screen_.find(live_, kPortraitHolder), kPanelStar), star);
    }
}

LotUi::Action LotUi::rightClicked(int window, LotLife& life) {
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
    screen_.setText(funds_, fundsText(funds));
    using ts2::sim::Speed;
    // The chosen speed stays pressed while paused. The pause button flips every half second.
    const auto now = std::chrono::steady_clock::now();
    if (paused && (!wasPaused_ || now - pauseBlink_ >= kPauseBlink)) {
        pauseLit_ = !wasPaused_ || !pauseLit_;
        pauseBlink_ = now;
    }
    wasPaused_ = paused;
    screen_.setChecked(speedButtons_[0], paused && pauseLit_);
    screen_.setChecked(speedButtons_[1], speed == Speed::Normal);
    screen_.setChecked(speedButtons_[2], speed == Speed::High);
    screen_.setChecked(speedButtons_[3], speed == Speed::Ultra);
    pauseFrame_.show(paused);
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
        // read in the background (asked for again next frame while too many are loading).
        const auto drawn = std::find_if(portraits_.begin(), portraits_.end(), [&](const auto& p) { return life.neighborId(p.first) == neighbor; });
        if (drawn != portraits_.end()) {
            pictures.make(neighbor, drawn->second);
        } else if (const auto file = life.neighborPortrait(neighbor)) {
            pictures.load(neighbor, file->file, file->instance);
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
// set (row 1, or 2 for a female Sim, with the target put in, and row 3 when there is no target or
// a token whose source isn't read yet), then its aspiration and influence values. Then the
// description (row 4).
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
    if (kind == WantArgument::Sim && w.argument >= 1 && w.argument <= INT16_MAX) {
        if (const auto neighbor = life.neighborName(static_cast<std::int16_t>(w.argument))) {
            name = row(neighbor->female ? 2 : 1);
            name = name.empty() ? row(1) : name;
            name = replaceAll(name, "$Neighbor", neighbor->first);
        }
    } else if (kind == WantArgument::Object) {
        name = replaceAll(row(1), "$Object", life.objectName(static_cast<std::uint32_t>(w.argument)));
    } else if (kind == WantArgument::None) {
        // Row 1 only when it has a value to put in, which isn't read yet.
        name = row(1).find('$') == std::string::npos ? std::string() : row(1);
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
    // index, else 1) of the object's group. Objects without that image are rendered in 3D. We only
    // use the drawings the game cached, filed by the object's model name (list 0x85 entry at the
    // named index, else at the model index, the 32-bit value in fields 0x48 and 0x49).
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
    if (!image) {
        if (!objectThumbnails_) {
            objectThumbnails_.emplace(objectThumbnailsFile_);
        }
        if (const auto name = code.string(type->group, kModelNames, named != 0 ? named : model)) {
            image = objectThumbnails_->picture(type->group, *name, kMaxPictureSide);
        }
    }
    return image;
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
    if (!hidden_) {
        showOptions(optionsShown());
    }
}

void LotUi::showLife(LotLife& life) {
    ts2::diag::Timer timer("lot ui");
    show(life.clockText(), life.speed(), life.paused(), life.funds());
    const std::int16_t sim = life.selected();
    skewer_.select(sim);
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
        const int holder = screen_.find(live_, kPortraitHolder);
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
    constexpr std::size_t kUiIconFlags = 0x34, kRoommateFlags = 0x54;
    for (const std::int16_t member : life.household()) {
        skewer_.setMood(member, life.mood(member));
        const auto data = life.personData(member);
        const auto memberPersonData = [&](std::size_t index) {
            return index < data.size() ? static_cast<std::uint16_t>(data[index]) : std::uint16_t{0};
        };
        skewer_.setAwayIcons(member, memberPersonData(kUiIconFlags), (memberPersonData(kRoommateFlags) & 0x800) != 0);
    }
    showNames(life, sim);
    showQueue(life, sim);
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
        screen_.setTip(screen_.find(live_, kPortraitHolder), sim != 0 ? fullName(life, sim) : std::string());
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
