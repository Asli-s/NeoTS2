#include "game_options_panel.hpp"

#include "content/install.hpp"
#include "content/user_props.hpp"
#include "diag/log.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>

#include <iterator>
#include <string_view>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11, kGameOptions = 0x49060F02;
// The settings groups' ids run from 0xA0 to 0xAC.
constexpr std::uint32_t kFirstGroup = 0xA0, kLastGroup = 0xAC;
// A settings group has at most this many buttons, with ids from 1.
constexpr std::uint32_t kMaxButtons = 16;
// Buttons that open things NeoTS2 doesn't have yet: the custom content list, the unlocked assets
// and Sims gift codes (shown with H&M Fashion Stuff), and Default Settings.
constexpr std::uint32_t kCustomContentList = 0x2FBC027E, kGiftCodes = 0x719A685F, kDefaultSettings = 0xAABBCCDD;
// The options menu's Game Options button.
constexpr std::uint32_t kMenuButton = 0x0000A243;
// The House-Specific Options frames: the one used on a lot and the greyed one used in the
// neighborhood, each with a Free Will On and Off button.
constexpr std::uint32_t kFreeWillFrame = 0xEDFA259B, kFreeWillGreyedFrame = 0xEDFA25B0;
constexpr std::uint32_t kFreeWillButtons[] = {0xCDFA287F, 0xCDFA2884};  // On, Off
constexpr std::uint32_t kGreyedFreeWillButtons[] = {0x2DFA25D9, 0x0DFA25C6};

// The settings NeoTS2 acts on. The rest are shown and saved but change nothing yet.
// force12HrTime only changes the clock in languages written with a 24-hour clock, and NeoTS2's
// clock is always in English, which the game writes with a 12-hour clock either way.
bool worksInNeoTs2(const std::string& key) {
    return key == "rotateCameraSims1Mode" || key == "force12HrTime" || key == "simInBackground" || key == "LotSkirtSizeIncrease" ||
           key == "EnableLotImpostersInLot" || key == "EnableNeighborhoodOccupantsInLot" || key == "neighborhoodPropFadeDistance";
}

// The widest skirt the game's settings ask for is 36 tiles. A larger number in a broken file is cut
// to this, so the skirt stays a size the neighborhood can hold.
constexpr int kMaxSkirtMargin = 256;
// The largest Fade Distance the game offers is 500. A larger number in a broken file is cut to
// this, so its square still fits the 32-bit number the game keeps it in.
constexpr unsigned long kMaxFadeDistance = 65535;

// Lot Imposters, not in the original game, in the Lot View Options' free room under Neighbors. Its rows are
// the game's own imposters, sharper ones (greyed until they can be made) and real houses.
constexpr std::uint32_t kNeighborsGroup = 0xA7, kOriginalRow = 1, kHighRow = 2, kRealHousesRow = 3;
constexpr int kImposterX = 118, kImposterY = 76, kImposterWidth = 170, kImposterHeight = 68;
constexpr int kImposterRowTop = 18, kImposterRowStep = 16, kImposterRowWidth = 110, kImposterRowHeight = 15;
const std::string kRealHousesKey = "lotRealHouses";
const std::string kFadeDistanceKey = "neighborhoodPropFadeDistance";
const std::string kHighImpostersKey = "lotHighImposters";

const std::string kNotWorking = "Not working in NeoTS2 yet. The choice is saved, but changes nothing.";
const std::string kButtonNotWorking = "Not working in NeoTS2 yet.";

} // namespace

void GameOptionsPanel::readSettings(const std::filesystem::path& install, const std::filesystem::path& userFolder) {
    userFolder_ = userFolder;
    saved_ = ts2::content::readUserProps(userFolder).values;
    const auto neo = ts2::content::readNeoProps(userFolder);
    imposters_ = neo.flag(kRealHousesKey).value_or(false)      ? LotImposters::RealHouses
                 : neo.flag(kHighImpostersKey).value_or(false) ? LotImposters::High
                                                               : LotImposters::Original;
    defaults_.clear();
    if (const auto file = ts2::content::newestDataFile(ts2::content::findProducts(install), "Res/Config/globalProps.xml")) {
        defaults_ = ts2::content::readPropsFile(*file).values;
    }
    graphicsRules_ = ts2::content::readGraphicsRules(install);
    // On until a lot is open, as globalProps.xml's freeWill says.
    freeWill_ = flag("freeWill", true);
}

void GameOptionsPanel::takeSettings(const GameOptionsPanel& other) {
    userFolder_ = other.userFolder_;
    saved_ = other.saved_;
    defaults_ = other.defaults_;
    graphicsRules_ = other.graphicsRules_;
    imposters_ = other.imposters_;
    showChoices();
}

void GameOptionsPanel::load(Place place) {
    remove();
    panel_ = screen_.load(kScripts, kGameOptions, 0, 0, menu_.menu());
    if (panel_ < 0) {
        ts2::diag::warnOnce("game options: the panel's script is missing, so Game Options opens nothing");
        return;
    }
    screen_.setVisible(panel_, false);
    menu_.place(panel_);
    for (std::uint32_t id = kFirstGroup; id <= kLastGroup; ++id) {
        const int group = screen_.find(panel_, id);
        if (group < 0) {
            continue;
        }
        auto control = ts2::content::parseOptionCaption(screen_.text(group));
        if (!control || control->values.size() > kMaxButtons) {
            continue;
        }
        groups_.push_back({group, std::move(*control)});
        const std::string& key = groups_.back().control.key;
        for (std::size_t button = 1; !worksInNeoTs2(key) && button <= groups_.back().control.values.size(); ++button) {
            screen_.setTip(screen_.find(group, static_cast<std::uint32_t>(button)), kNotWorking);
        }
    }
    addImposterGroup();
    for (const std::uint32_t id : {kCustomContentList, kGiftCodes, kDefaultSettings}) {
        screen_.setTip(screen_.find(panel_, id), kButtonNotWorking);
    }
    screen_.setVisible(screen_.find(panel_, kFreeWillFrame), place == Place::Lot);
    screen_.setVisible(screen_.find(panel_, kFreeWillGreyedFrame), place == Place::Neighborhood);
    for (const std::uint32_t id : kGreyedFreeWillButtons) {
        screen_.setEnabled(screen_.find(panel_, id), false);
    }
    // Free Will shows the lot's own choice (see setFreeWill). Picking one sets the lot's Free Will
    // global, which the lot saves.
    showFreeWill();
    showChoices();
}

void GameOptionsPanel::addImposterGroup() {
    const int neighbors = screen_.find(panel_, kNeighborsGroup);
    if (neighbors < 0) {
        return;
    }
    const ts2::ui::Window group{{{"iid", "IGZWinGen"}, {"winflag_visible", "yes"}, {"area", std::format("({},{},{},{})", kImposterX, kImposterY, kImposterX + kImposterWidth, kImposterY + kImposterHeight)}}, {}};
    imposterGroup_ = screen_.addWindows({group}, kImposterX, kImposterY, screen_.parent(neighbors));
    // Made like the panel's own titles and radio buttons in its script.
    const ts2::ui::Window title{{{"iid", "IGZWinBtn"}, {"area", std::format("(0,0,{},20)", kImposterWidth)}, {"winflag_visible", "yes"}, {"winflag_ignoremouse", "yes"},
                                 {"font", "OptionsText"}, {"colorfontnormal", "(5,6,113)"}, {"showcaption", "yes"}, {"gutters", "(2,0,0,0)"}, {"align", "left"}},
                                {}};
    screen_.setText(screen_.addWindows({title}, 0, 0, imposterGroup_), "Lot Imposters");
    const char* const rows[] = {"Original", "High", "Real Houses"};
    for (std::uint32_t id = kOriginalRow; id <= kRealHousesRow; ++id) {
        const int top = kImposterRowTop + static_cast<int>(id - kOriginalRow) * kImposterRowStep;
        const ts2::ui::Window row{{{"iid", "IGZWinBtn"}, {"id", std::format("0x{:08x}", id)},
                                   {"area", std::format("(0,{},{},{})", top, kImposterRowWidth, top + kImposterRowHeight)},
                                   {"winflag_visible", "yes"}, {"winflag_enabled", "yes"}, {"winflag_alphablend", "yes"}, {"image", "{499db772,ac76e165}"},
                                   {"font", "OptionsText"}, {"colorfontnormal", "(5,6,113)"}, {"colorfontdisabled", "(84,85,105)"}, {"colorfonthilited", "(204,214,255)"}, {"showcaption", "yes"},
                                   {"fill", "yes"}, {"style", "radiocheck"}, {"gutters", "(0,2,0,2)"}, {"align", "center"}, {"btnclicksnd", "{00000000,ff391ba9}"}},
                                  {}};
        screen_.setText(screen_.addWindows({row}, 0, top, imposterGroup_), rows[id - kOriginalRow]);
    }
    screen_.setTip(screen_.find(imposterGroup_, kHighRow), "Every lot's imposter is drawn again from the lot, four times sharper, while the neighborhood is open. "
                                                           "The pictures are kept in the NeoTS2Cache folder.");
    screen_.setTip(screen_.find(imposterGroup_, kRealHousesRow), "The nearest neighbors around a lot are real houses instead of flat imposters, and nothing fades. It takes much more memory.");
}

void GameOptionsPanel::chooseImposters(LotImposters kind) {
    if (kind == imposters_) {
        return;
    }
    imposters_ = kind;
    changed_ = true;
    const auto flag = [](bool on) { return std::pair<std::string, bool>{on ? "true" : "false", true}; };
    if (!ts2::content::saveNeoProps(userFolder_, {{kRealHousesKey, flag(kind == LotImposters::RealHouses)},
                                                  {kHighImpostersKey, flag(kind == LotImposters::High)}})) {
        ts2::diag::warnOnce("game options: Lot Imposters could not be saved");
    }
}

void GameOptionsPanel::showFreeWill() {
    if (panel_ < 0) {
        return;
    }
    for (std::size_t i = 0; i < std::size(kFreeWillButtons); ++i) {
        screen_.setChecked(screen_.find(panel_, kFreeWillButtons[i]), (i == 0) == freeWill_);
    }
}

std::optional<bool> GameOptionsPanel::takeFreeWill() {
    const auto picked = pickedFreeWill_;
    pickedFreeWill_.reset();
    return picked;
}

void GameOptionsPanel::remove() {
    if (panel_ >= 0) {
        if (shown()) {
            menu_.close();
        }
        screen_.remove(panel_);
    }
    panel_ = -1;
    imposterGroup_ = -1;
    groups_.clear();
}

void GameOptionsPanel::show(bool show) {
    if (panel_ < 0 || show == shown()) {
        return;
    }
    if (show) {
        showChoices();
        menu_.open(panel_, kMenuButton);
    } else {
        menu_.close();
    }
}

void GameOptionsPanel::showChoices() {
    for (const Group& g : groups_) {
        const auto saved = saved_.find(g.control.savedKey());
        std::optional<std::size_t> chosen;
        if (saved != saved_.end()) {
            chosen = g.control.buttonFor(saved->second);
        } else if (g.control.kind == ts2::content::OptionControl::Kind::Radio) {
            if (const auto fallback = defaults_.find(g.control.key); fallback != defaults_.end()) {
                chosen = g.control.buttonFor(fallback->second);
            }
        }
        // Real houses are drawn whole, so the Fade Distance is off and greyed while they are chosen.
        const bool greyed = g.control.key == kFadeDistanceKey && imposters_ == LotImposters::RealHouses;
        for (std::size_t button = 1; button <= g.control.values.size(); ++button) {
            const int w = screen_.find(g.window, static_cast<std::uint32_t>(button));
            screen_.setChecked(w, chosen == button);
            screen_.setEnabled(w, !greyed);
        }
    }
    if (panel_ >= 0 && imposterGroup_ >= 0) {
        screen_.setChecked(screen_.find(imposterGroup_, kOriginalRow), imposters_ == LotImposters::Original);
        screen_.setChecked(screen_.find(imposterGroup_, kHighRow), imposters_ == LotImposters::High);
        screen_.setChecked(screen_.find(imposterGroup_, kRealHousesRow), imposters_ == LotImposters::RealHouses);
    }
}

bool GameOptionsPanel::clicked(int window) {
    if (panel_ < 0 || window < 0) {
        return false;
    }
    // The button opens its panel, or closes it when it is the one open, as each panel button does.
    if (window == menu_.button(kMenuButton)) {
        show(!shown());
        return true;
    }
    if (imposterGroup_ >= 0 && screen_.parent(window) == imposterGroup_) {
        const std::uint32_t row = screen_.id(window);
        if (row == kOriginalRow || row == kHighRow || row == kRealHousesRow) {
            chooseImposters(row == kRealHousesRow ? LotImposters::RealHouses : row == kHighRow ? LotImposters::High : LotImposters::Original);
            showChoices();
        }
        return true;
    }
    for (const Group& g : groups_) {
        if (screen_.parent(window) != g.window) {
            continue;
        }
        const std::uint32_t button = screen_.id(window);
        if (button < 1 || button > g.control.values.size()) {
            return true;
        }
        const std::string value = g.control.savedValue(button);
        const std::string key = g.control.savedKey();
        if (saved_[key] != value) {
            saved_[key] = value;
            changed_ = true;
            const bool isBool = g.control.kind == ts2::content::OptionControl::Kind::Radio && g.control.isBool;
            if (!ts2::content::saveUserProps(userFolder_, {{key, {value, isBool}}})) {
                ts2::diag::warnOnce("game options: a choice could not be saved to userProps.xml");
            }
        }
        showChoices();
        return true;
    }
    for (std::size_t i = 0; i < std::size(kFreeWillButtons); ++i) {
        if (window == screen_.find(panel_, kFreeWillButtons[i])) {
            freeWill_ = i == 0;
            pickedFreeWill_ = freeWill_;
            changed_ = true;
            showFreeWill();
            return true;
        }
    }
    for (int w = window; w >= 0; w = screen_.parent(w)) {
        if (w == panel_) {
            return true;
        }
    }
    return false;
}

bool GameOptionsPanel::takeChanged() {
    const bool changed = changed_;
    changed_ = false;
    return changed;
}

GameOptionsPanel::LotView GameOptionsPanel::lotView() const {
    // The game sets these properties from the options' saved settings through Graphics Rules, and
    // without a saved setting globalProps.xml's values stand.
    const ts2::content::UserProps saved{saved_};
    const auto prop = [&](std::string_view option, const std::string& key) -> std::optional<std::string> {
        if (auto value = ts2::content::savedOptionProp(saved, graphicsRules_, option, key)) {
            return value;
        }
        const auto it = defaults_.find(key);
        return it == defaults_.end() ? std::nullopt : std::optional(it->second);
    };
    LotView out;
    if (const auto margin = prop("LotSkirtSizeIncrease", "lotSkirtSizeIncrease")) {
        // The game keeps at least one tile.
        const int tiles = std::atoi(margin->c_str());
        out.skirtMargin = std::clamp(tiles, 1, kMaxSkirtMargin);
    }
    out.neighbors = prop("EnableLotImpostersInLot", "enableLotImpostersInLot").value_or("true") != "false";
    out.decorations = prop("EnableNeighborhoodOccupantsInLot", "enableNeighborhoodOccupantsInLot").value_or("true") != "false";
    out.realHouses = imposters_ == LotImposters::RealHouses && out.neighbors;
    out.highImposters = imposters_ == LotImposters::High;
    // Saved as the distance itself. Without one, the game starts from 50.
    for (const auto* values : {&saved_, &defaults_}) {
        if (const auto it = values->find("neighborhoodPropFadeDistance"); it != values->end()) {
            const unsigned long distance = std::min(std::strtoul(it->second.c_str(), nullptr, 10), kMaxFadeDistance);
            out.fadeSquared = static_cast<float>(distance) * static_cast<float>(distance);
            break;
        }
    }
    if (out.realHouses) {
        out.fadeSquared = 0;
    }
    return out;
}

bool GameOptionsPanel::flag(const std::string& key, bool fallback) const {
    for (const auto* values : {&saved_, &defaults_}) {
        if (const auto it = values->find(key); it != values->end()) {
            return it->second == "true";
        }
    }
    return fallback;
}

} // namespace app
