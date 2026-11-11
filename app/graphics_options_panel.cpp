#include "graphics_options_panel.hpp"

#include "content/user_props.hpp"
#include "diag/log.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <iterator>

namespace app {

namespace {

using ts2::content::GraphicsGroup;
using ts2::content::kGraphicsGroups;
using ts2::content::kSmoothEdgeSamples;

constexpr std::uint32_t kScripts = 0xA99D8A11, kGraphicsOptions = 0x49060F03;
// The options menu's Graphics button.
constexpr std::uint32_t kMenuButton = 0x0000A241;
constexpr std::uint32_t kApply = 0x2D1A5296, kDefaultSettings = 0xAABBCCDD;
// Use Square Pixels was for old screen modes stretched on wide screens, so NeoTS2 has Fullscreen in
// its place. Its group's title is the group's only child without an id.
constexpr std::uint32_t kFullscreenGroup = 0x10, kFullscreenOff = 0x11, kFullscreenOn = 0x12;
const std::string kFullscreenKey = "fullscreen", kFrameLimitKey = "frameLimit", kDisplayModeKey = "activeDeviceDisplayMode";
// The lists, in their script windows. The refresh rate list is NeoTS2's frame rate list, as a window
// has no refresh rate to pick.
constexpr std::uint32_t kScreenSizeList = 0x35, kFrameRateList = 0x45;
// The smallest screen the game lays its UI out for.
constexpr int kMinWidth = 800, kMinHeight = 600;
// The Frame Rate list's rows tied to the screen's refresh rate, then fixed rates, then Unlimited.
struct ScreenRate {
    const char* text;
    int limit;
    int refreshes;  // a frame shows for
};
constexpr ScreenRate kScreenRates[] = {{"Screen", GraphicsOptionsPanel::kWaitForScreen, 1},
                                       {"Half", GraphicsOptionsPanel::kHalfScreen, 2},
                                       {"Third", GraphicsOptionsPanel::kThirdScreen, 3}};
constexpr int kFixedRates[] = {30, 60, 90, 120, 144};
bool knownFrameLimit(int limit) {
    return limit == GraphicsOptionsPanel::kNoLimit || std::any_of(std::begin(kScreenRates), std::end(kScreenRates), [&](const ScreenRate& r) { return r.limit == limit; }) ||
           std::find(std::begin(kFixedRates), std::end(kFixedRates), limit) != std::end(kFixedRates);
}
// The Shadows group's title. The game greys it with its buttons.
constexpr std::uint32_t kShadowsTitle = 0x94;
constexpr std::uint32_t kFrameRateTitle = 0xAD7FAA35;
// Smooth Edges. The game's slider runs over the levels a card has, so one place on it means a different
// level on each card. NeoTS2 names the levels with radio buttons in its place, in the panel's other
// groups' rows, and keeps the choice as its own setting.
constexpr std::uint32_t kSmoothEdgesGroup = 0xC0, kSmoothEdgesBar = 0xECD97AFE, kSmoothEdgesLess = 0x2E10C53D, kSmoothEdgesMore = 0x2E10C541;
constexpr std::uint32_t kFirstSmoothEdgeRow = 0xC8;
// Ambient Occlusion, not in the original game, and its quality, Full Size or Half Size.
constexpr std::uint32_t kOcclusionGroup = 0xD3, kFirstOcclusionRow = 0xCC;
constexpr std::uint32_t kOcclusionSizeGroup = 0xD2, kOcclusionFullSize = 0xD0, kOcclusionHalfSize = 0xD1;
// Water and Sky, not in the original game: Classic as the game draws it, or Realistic.
constexpr std::uint32_t kWaterGroup = 0xE0, kSkyGroup = 0xE8, kClassicRow = 0xE1, kRealisticRow = 0xE2;
// Terrain Detail, not in the original game: the tiles one picture of the neighborhood ground's paints
// covers, the game's own 13 first. Fewer show more of each picture's detail, and repeat it more often.
constexpr std::uint32_t kTerrainGroup = 0xEC, kFirstTerrainRow = 0xF0;
constexpr float kTerrainTiles[] = {0, 9, 6.5f};
// A group's rows start under its title, and Smooth Edges' end over the group's bottom.
constexpr int kSmoothEdgeRowTop = 15, kFirstRowTop = 24, kRowHeight = 16, kRowWidth = 83;
constexpr int kGroupWidth = 163, kSmoothEdgesHeight = 86;
// NeoTS2's own groups stand under the panel's in its three columns, this far apart, and the panel's
// lowest groups move down below the tallest column.
constexpr int kGroupGap = 4;
// What sits lower than this in the panel's script moves down, and the lists' parts start at the list top.
constexpr int kLowerGroupsTop = 300;
// The panel's picture, stretched only in a plain band of it, so its corners and title keep their shape.
constexpr std::uint32_t kPictureWidth = 681, kPictureHeight = 430, kBandTop = 300, kBandBottom = 304;
const std::string kPanelPicture = "{499db772,dafe9000}";
constexpr std::uint32_t kOffText = 0x00800003;  // string id, leakcheck: ok
constexpr std::uint32_t kMediumText = 0x00800005;  // string id, leakcheck: ok
// UI Size, not in the original game, in the free room under the Frame Rate list.
constexpr std::uint32_t kUiSizeGroup = 0xD7, kFirstUiSizeRow = 0xD8;
constexpr int kUiSizeGap = 4;
const std::string kUiScaleKey = "uiScale", kWaterKey = "realisticWater", kSkyKey = "realisticSky";
// Older versions kept Pond Water and Sea Water apart.
const std::string kOldPondsKey = "realisticPonds", kOldSeaKey = "realisticSea";
const std::string kTerrainKey = "terrainDetail";
const std::string kSmoothEdgesKey = "smoothEdges", kOcclusionKey = "ambientOcclusion", kOcclusionSizeKey = "ambientOcclusionHalfSize";

// The keep-these-settings box: its text, numbered within the Options text's string table 0x80, and
// the title, Yes and No it shares with the quit box.
constexpr std::uint32_t kOptionsText = 0x7F232750, kKeepText = 0x00800010;   // string id, leakcheck: ok
constexpr std::uint32_t kUiText = 0x7FA7DC7C, kTitle = 0x00010012;           // string id, leakcheck: ok
constexpr std::uint32_t kYesText = 0x008E0003, kNoText = 0x008E0004;         // string ids, leakcheck: ok
constexpr std::uint32_t kYes = 0x5301814C, kNo = 0x53018149;
// The box counts down from this, one a second, and goes back at 0.
constexpr int kCountdown = 16;
constexpr std::uint64_t kCountMs = 1000;

std::string savedKey(const GraphicsGroup& group) {
    return "option" + std::string(group.option);
}

const std::string kNotWorking = "Not working in NeoTS2 yet. The choice is saved, but changes nothing.";
const std::string kOnlyBumps = "In NeoTS2 this only changes bump maps so far. The rest of it is not working yet.";
// The options that act in full. UI Transparency changes nothing that shows in the game either.
bool worksInFull(std::string_view option) {
    return option == "EffectsQuality" || option == "Shadows" || option == "LightingQuality" || option == "Reflection" || option == "OpaqueUI";
}

} // namespace

void GraphicsOptionsPanel::readSettings(ts2::content::UiData& data, const std::filesystem::path& install, const std::filesystem::path& userFolder) {
    userFolder_ = userFolder;
    graphicsRules_ = ts2::content::readGraphicsRules(install);
    const auto saved = ts2::content::readUserProps(userFolder).values;
    values_.clear();
    for (const auto& [key, value] : saved) {
        values_.emplace(key, value);
    }
    boxTitle_ = data.text(kUiText, kTitle);
    boxText_ = data.text(kOptionsText, kKeepText);
    yes_ = data.text(kUiText, kYesText);
    no_ = data.text(kUiText, kNoText);
    display_ = savedDisplay(userFolder);
    askedSize_ = {display_.width, display_.height};
}

GraphicsOptionsPanel::Display GraphicsOptionsPanel::savedDisplay(const std::filesystem::path& userFolder) {
    Display out;
    const auto mode = ts2::content::readUserProps(userFolder).values;
    if (const auto saved = mode.find(kDisplayModeKey); saved != mode.end()) {
        const char* text = saved->second.c_str();
        const char* end = text + saved->second.size();
        int width = 0, height = 0;
        const auto w = std::from_chars(text, end, width);
        if (w.ptr != end && *w.ptr == 'x' && std::from_chars(w.ptr + 1, end, height).ec == std::errc() && width >= kMinWidth && height >= kMinHeight) {
            out.width = width;
            out.height = height;
        }
    }
    const auto own = ts2::content::readNeoProps(userFolder);
    out.fullscreen = own.flag(kFullscreenKey).value_or(false);
    if (const auto saved = own.values.find(kFrameLimitKey); saved != own.values.end()) {
        int limit = 0;
        const char* end = saved->second.data() + saved->second.size();
        if (std::from_chars(saved->second.data(), end, limit).ptr == end && knownFrameLimit(limit)) {
            out.frameLimit = limit;
        }
    }
    if (const auto saved = own.values.find(kSmoothEdgesKey); saved != own.values.end()) {
        out.smoothEdges = ts2::content::savedSmoothEdges(saved->second);
    }
    if (const auto saved = own.values.find(kOcclusionKey); saved != own.values.end()) {
        out.ambientOcclusion = ts2::content::savedAmbientOcclusion(saved->second);
    }
    out.occlusionHalfSize = own.flag(kOcclusionSizeKey).value_or(true);
    // Either of the older options Realistic makes Water so.
    out.realisticWater = own.flag(kWaterKey).value_or(own.flag(kOldPondsKey).value_or(false) || own.flag(kOldSeaKey).value_or(false));
    out.realisticSky = own.flag(kSkyKey).value_or(false);
    if (const auto saved = own.values.find(kTerrainKey); saved != own.values.end()) {
        int detail = 0;
        const char* end = saved->second.data() + saved->second.size();
        if (std::from_chars(saved->second.data(), end, detail).ptr == end && detail >= 0 && detail < static_cast<int>(std::size(kTerrainTiles))) {
            out.terrainDetail = detail;
        }
    }
    if (const auto saved = own.values.find(kUiScaleKey); saved != own.values.end()) {
        int scale = 0;
        const char* end = saved->second.data() + saved->second.size();
        if (std::from_chars(saved->second.data(), end, scale).ptr == end && ts2::content::isUiScale(scale)) {
            out.uiScale = scale;
        }
    }
    return out;
}

void GraphicsOptionsPanel::takeSettings(const GraphicsOptionsPanel& other) {
    userFolder_ = other.userFolder_;
    graphicsRules_ = other.graphicsRules_;
    values_ = other.values_;
    boxTitle_ = other.boxTitle_;
    boxText_ = other.boxText_;
    yes_ = other.yes_;
    no_ = other.no_;
    display_ = other.display_;
    askedSize_ = other.askedSize_;
    mostSmoothEdges_ = other.mostSmoothEdges_;
    showChoices();
}

void GraphicsOptionsPanel::load(Place place) {
    remove();
    panel_ = screen_.load(kScripts, kGraphicsOptions, 0, 0, menu_.menu());
    if (panel_ < 0) {
        ts2::diag::warnOnce("graphics options: the panel's script is missing, so Graphics opens nothing");
        return;
    }
    screen_.setVisible(panel_, false);
    menu_.place(panel_);
    const bool shadows = place == Place::Lot;
    screen_.setEnabled(screen_.find(panel_, kShadowsTitle), shadows);
    for (const auto& choice : kGraphicsGroups[0].choices) {
        screen_.setEnabled(screen_.find(screen_.find(panel_, kGraphicsGroups[0].window), choice.button), shadows);
    }
    screen_.setText(screen_.find(panel_, kFrameRateTitle), "Frame Rate");
    const int fullscreen = screen_.find(panel_, kFullscreenGroup);
    for (const int part : screen_.children(fullscreen)) {
        if (screen_.id(part) == 0) {
            screen_.setText(part, "Fullscreen");
        }
    }
    // The script's lowest groups, which move down under NeoTS2's own.
    std::vector<int> lowest;
    for (const int part : screen_.children(panel_)) {
        if (screen_.y(part) > kLowerGroupsTop) {
            lowest.push_back(part);
        }
    }
    const int sizeList = screen_.find(panel_, kScreenSizeList), rateList = screen_.find(panel_, kFrameRateList);
    // The Frame Rate list as wide as the Screen Size list, for its longer rows. Its group widens with it.
    if (sizeList >= 0 && rateList >= 0 && screen_.width(rateList) < screen_.width(sizeList)) {
        const int group = screen_.parent(rateList);
        // The list's background is a picture of the same area beside it in the script.
        for (const int part : screen_.children(group)) {
            if (screen_.x(part) == screen_.x(rateList) && screen_.y(part) == screen_.y(rateList) && screen_.width(part) == screen_.width(rateList) &&
                screen_.height(part) == screen_.height(rateList)) {
                screen_.resize(part, screen_.width(sizeList), screen_.height(part));
            }
        }
        const int extra = screen_.x(rateList) + screen_.width(sizeList) - screen_.width(group);
        if (group >= 0 && group != panel_ && extra > 0) {
            screen_.resize(group, screen_.width(group) + extra, screen_.height(group));
        }
    }
    screenSizes_.attach(screen_, sizeList);
    frameRates_.attach(screen_, rateList);
    listFrameRates();
    listScreenSizes();
    addSmoothEdgeRows();
    addUiSizeRows();
    makeTaller(lowest, addOwnGroups());
    for (const GraphicsGroup& group : kGraphicsGroups) {
        const int window = screen_.find(panel_, group.window);
        const bool bumps = group.option == "MaterialDetail";
        for (int i = 0; !worksInFull(group.option) && i < group.count; ++i) {
            screen_.setTip(screen_.find(window, group.choices[static_cast<std::size_t>(i)].button), bumps ? kOnlyBumps : kNotWorking);
        }
    }
    showChoices();
}

void GraphicsOptionsPanel::makeTaller(const std::vector<int>& lowest, int bottom) {
    const std::vector<int> parts = screen_.children(panel_);
    if (parts.size() < 2 || lowest.empty()) {
        return;
    }
    const int picture = parts[0], above = parts[1];
    int top = screen_.y(lowest.front());
    for (const int part : lowest) {
        top = std::min(top, screen_.y(part));
    }
    const int extra = std::max(0, bottom + kGroupGap - top);
    for (const int part : lowest) {
        screen_.move(part, screen_.x(part), screen_.y(part) + extra);
    }
    screen_.resize(panel_, screen_.width(panel_), screen_.height(panel_) + extra);
    // The picture in three: the top as it is, the band stretched over the extra height, the bottom as
    // it is. Each lets the mouse through where it is clear, as the script's picture does.
    const int width = static_cast<int>(kPictureWidth);
    const auto addPart = [&](int top, int height, int partTop, int partBottom) {
        const ts2::ui::Window window{{{"iid", "IGZWinBMP"}, {"area", std::format("(0,{},{},{})", top, width, top + height)}, {"winflag_visible", "yes"}, {"winflag_mousetrans", "yes"},
                                      {"winflag_pbuff", "yes"}, {"winflag_pbufftrans", "yes"}, {"winflag_alphablend", "yes"}, {"image", kPanelPicture}},
                                     {}};
        const int made = screen_.addWindows({window}, 0, top, panel_);
        screen_.setImagePart(made, {0, partTop, static_cast<int>(kPictureWidth), partBottom});
        screen_.placeUnder(made, above);
    };
    // The band is stretched from its middle rows only, as smoothing at its edges would blend in the
    // rows around it, and the dark top of the bar's continuation below would show as a strip.
    addPart(kBandTop, kBandBottom - kBandTop + extra, kBandTop + 1, kBandBottom - 1);
    addPart(kBandBottom + extra, kPictureHeight - kBandBottom, kBandBottom, kPictureHeight);
    screen_.resize(picture, width, kBandTop);
    screen_.setImagePart(picture, {0, 0, static_cast<int>(kPictureWidth), kBandTop});
}

void GraphicsOptionsPanel::addGroupTitle(int group, int top, const std::string& text) {
    // As Smooth Edges' title is in the script.
    const ts2::ui::Window title{{{"iid", "IGZWinBtn"}, {"area", std::format("(0,{},155,{})", top, top + 30)}, {"winflag_visible", "yes"}, {"winflag_ignoremouse", "yes"},
                                 {"font", "OptionsText"}, {"colorfontnormal", "(5,6,113)"}, {"showcaption", "yes"}, {"gutters", "(3,0,0,0)"}, {"align", "left"}},
                                {}};
    screen_.setText(screen_.addWindows({title}, 0, top, group), text);
}

int GraphicsOptionsPanel::addRadioRow(int group, std::uint32_t id, int top, const std::string& text, int left) {
    // As the panel's other radio buttons are in its script.
    const ts2::ui::Window row{{{"iid", "IGZWinBtn"}, {"id", std::format("0x{:08x}", id)}, {"area", std::format("({},{},{},{})", left, top, left + kRowWidth, top + kRowHeight)},
                               {"winflag_visible", "yes"}, {"winflag_enabled", "yes"}, {"winflag_alphablend", "yes"}, {"image", "{499db772,ac76e165}"},
                               {"font", "OptionsText"}, {"colorfontnormal", "(5,6,113)"}, {"colorfontdisabled", "(84,85,105)"}, {"colorfonthilited", "(204,214,255)"}, {"showcaption", "yes"},
                               {"fill", "yes"}, {"style", "radiocheck"}, {"gutters", "(0,2,0,2)"}, {"align", "center"}, {"btnclicksnd", "{00000000,ff391ba9}"}},
                              {}};
    const int window = screen_.addWindows({row}, left, top, group);
    screen_.setText(window, text);
    return window;
}

void GraphicsOptionsPanel::addUiSizeRows() {
    const int list = screen_.find(panel_, kFrameRateList);
    if (list < 0) {
        return;
    }
    // Just under the list, in the panel's own pixels.
    int left = 0, top = screen_.height(list);
    for (int w = list; w >= 0 && w != panel_; w = screen_.parent(w)) {
        left += screen_.x(w);
        top += screen_.y(w);
    }
    std::vector<std::pair<std::uint32_t, std::string>> rows;
    for (std::size_t i = 0; i < std::size(ts2::content::kUiScales); ++i) {
        rows.emplace_back(kFirstUiSizeRow + static_cast<std::uint32_t>(i), std::format("{}%", ts2::content::kUiScales[i]));
    }
    addChoiceGroup(kUiSizeGroup, left, top + kUiSizeGap, "UI Size", rows);
}

int GraphicsOptionsPanel::addChoiceGroup(std::uint32_t id, int x, int y, const std::string& title, const std::vector<std::pair<std::uint32_t, std::string>>& rows) {
    const int height = kFirstRowTop + static_cast<int>(rows.size()) * kRowHeight;
    const ts2::ui::Window window{{{"iid", "IGZWinGen"}, {"id", std::format("0x{:08x}", id)}, {"winflag_visible", "yes"},
                                  {"area", std::format("(0,0,{},{})", kGroupWidth, height)}},
                                 {}};
    const int group = screen_.addWindows({window}, x, y, panel_);
    addGroupTitle(group, 0, title);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        addRadioRow(group, rows[i].first, kFirstRowTop + static_cast<int>(i) * kRowHeight, rows[i].second);
    }
    return group;
}

int GraphicsOptionsPanel::addOwnGroups() {
    // Ambient Occlusion and AO Quality under Screen Size, and Sky, Water and Terrain Detail under
    // Smooth Edges. UI Size has the middle column to itself.
    const auto place = [&](int window) {
        std::array<int, 2> at{0, screen_.height(window)};
        for (int w = window; w >= 0 && w != panel_; w = screen_.parent(w)) {
            at[0] += screen_.x(w);
            at[1] += screen_.y(w);
        }
        return at;
    };
    int bottom = 0;
    using Rows = std::vector<std::pair<std::uint32_t, std::string>>;
    const auto under = [&](int window, std::uint32_t id, const std::string& title, const Rows& rows) {
        if (window < 0) {
            return -1;
        }
        const auto at = place(window);
        const int group = addChoiceGroup(id, at[0], at[1] + kGroupGap, title, rows);
        bottom = std::max(bottom, place(group)[1]);
        return group;
    };
    const Rows styles{{kClassicRow, "Classic"}, {kRealisticRow, "Realistic"}};
    const std::string off = screen_.data().text(kOptionsText, kOffText);
    Rows levels{{kFirstOcclusionRow, off}, {kFirstOcclusionRow + 1, "Light"}, {kFirstOcclusionRow + 2, screen_.data().text(kOptionsText, kMediumText)},
                {kFirstOcclusionRow + 3, "Dark"}};
    // Placed under the Screen Size list as UI Size is under the Frame Rate list, so the two line up.
    const int occlusion = under(screen_.find(panel_, kScreenSizeList), kOcclusionGroup, "Ambient Occlusion", levels);
    under(occlusion, kOcclusionSizeGroup, "AO Quality", {{kOcclusionFullSize, "Full Size"}, {kOcclusionHalfSize, "Half Size"}});
    const int sky = under(screen_.find(panel_, kSmoothEdgesGroup), kSkyGroup, "Sky", styles);
    const int water = under(sky, kWaterGroup, "Water", styles);
    std::vector<std::pair<std::uint32_t, std::string>> steps;
    for (std::size_t i = 0; i < std::size(kTerrainTiles); ++i) {
        steps.emplace_back(kFirstTerrainRow + static_cast<std::uint32_t>(i), i == 0 ? "Classic" : i == 1 ? "Fine" : "Finer");
    }
    const int terrain = under(water, kTerrainGroup, "Terrain Detail", steps);
    for (std::size_t i = 1; i < std::size(kTerrainTiles); ++i) {
        screen_.setTip(screen_.find(terrain, kFirstTerrainRow + static_cast<std::uint32_t>(i)),
                       "Not in the original game: the neighborhood's ground pictures are drawn smaller, so more of their detail shows, "
                       "and they repeat more often.");
    }
    screen_.setTip(screen_.find(sky, kRealisticRow), "Not in the original game: the game's skies recoloured. It shows the next time a lot or neighborhood opens.");
    screen_.setTip(screen_.find(water, kRealisticRow), "Not in the original game: ponds and the sea are drawn like pools, with waves, a colour that "
                                                      "deepens with the water and caustics, on lots and in the neighborhood.");
    return bottom;
}

float GraphicsOptionsPanel::terrainTiles(int detail) {
    return detail >= 0 && detail < static_cast<int>(std::size(kTerrainTiles)) ? kTerrainTiles[detail] : 0;
}

bool GraphicsOptionsPanel::ownGroup(int window) const {
    for (const std::uint32_t id : {kOcclusionGroup, kOcclusionSizeGroup, kWaterGroup, kSkyGroup, kTerrainGroup, kUiSizeGroup}) {
        if (window >= 0 && window == screen_.find(panel_, id)) {
            return true;
        }
    }
    return false;
}

void GraphicsOptionsPanel::addSmoothEdgeRows() {
    const int group = screen_.find(panel_, kSmoothEdgesGroup);
    for (const std::uint32_t part : {kSmoothEdgesBar, kSmoothEdgesLess, kSmoothEdgesMore}) {
        screen_.setVisible(screen_.find(group, part), false);
    }
    screen_.resize(group, kGroupWidth, kSmoothEdgesHeight);
    const std::string off = screen_.data().text(kOptionsText, kOffText);
    for (std::size_t i = 0; i < kSmoothEdgeSamples.size(); ++i) {
        addRadioRow(group, kFirstSmoothEdgeRow + static_cast<std::uint32_t>(i), kSmoothEdgeRowTop + static_cast<int>(i) * kRowHeight, i == 0 ? off : std::format("{}x", kSmoothEdgeSamples[i]));
    }
}

void GraphicsOptionsPanel::remove() {
    box_.close(screen_);
    if (panel_ >= 0) {
        if (shown()) {
            menu_.close();
        }
        screen_.remove(panel_);
    }
    panel_ = -1;
}

void GraphicsOptionsPanel::show(bool show) {
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

int GraphicsOptionsPanel::setting(const GraphicsGroup& group) const {
    const auto saved = values_.find(savedKey(group));
    if (saved != values_.end()) {
        int number = 0;
        const char* end = saved->second.data() + saved->second.size();
        if (std::from_chars(saved->second.data(), end, number).ptr == end) {
            return number;
        }
    }
    return group.highLevel;
}

bool GraphicsOptionsPanel::propOn(std::string_view option, std::string_view key) const {
    const auto* group = std::find_if(std::begin(kGraphicsGroups), std::end(kGraphicsGroups), [&](const GraphicsGroup& g) { return g.option == option; });
    if (group == std::end(kGraphicsGroups)) {
        return true;
    }
    const auto props = ts2::content::graphicsOptionProps(graphicsRules_, option, setting(*group));
    const auto prop = props.find(std::string(key));
    return prop == props.end() || prop->second != "false";
}

int GraphicsOptionsPanel::propNumber(std::string_view option, std::string_view key, int otherwise) const {
    const auto* group = std::find_if(std::begin(kGraphicsGroups), std::end(kGraphicsGroups), [&](const GraphicsGroup& g) { return g.option == option; });
    if (group == std::end(kGraphicsGroups)) {
        return otherwise;
    }
    const auto props = ts2::content::graphicsOptionProps(graphicsRules_, option, setting(*group));
    const auto prop = props.find(std::string(key));
    if (prop == props.end()) {
        return otherwise;
    }
    int value = otherwise;
    const auto [end, error] = std::from_chars(prop->second.data(), prop->second.data() + prop->second.size(), value);
    return error == std::errc{} && end == prop->second.data() + prop->second.size() ? value : otherwise;
}

void GraphicsOptionsPanel::checkFullscreen(bool on) {
    screen_.setChecked(screen_.find(panel_, kFullscreenOff), !on);
    screen_.setChecked(screen_.find(panel_, kFullscreenOn), on);
    // A full screen takes the screen's own size.
    screenSizes_.setEnabled(!on);
}

void GraphicsOptionsPanel::listScreenSizes() {
    sizes_.clear();
    std::vector<std::string> rows;
    for (const auto& mode : ts2::platform::firstWindowDisplayModes()) {
        // The game lists only sizes wider than tall, from the smallest its UI is laid out for. It
        // also stops at Graphics Rules' largest size, which held good screens back for years, and
        // at 100 Hz, so NeoTS2 doesn't.
        if (mode.width >= kMinWidth && mode.height >= kMinHeight && mode.width > mode.height) {
            sizes_.push_back({mode.width, mode.height});
            rows.push_back(std::format("{}x{}", mode.width, mode.height));
        }
    }
    screenSizes_.setRows(rows);
}

void GraphicsOptionsPanel::setScreenRefreshRate(double hz) {
    if (hz == refreshRate_) {
        return;
    }
    refreshRate_ = hz;
    if (panel_ >= 0) {
        const int picked = frameRates_.picked();
        const int limit = picked >= 0 && static_cast<std::size_t>(picked) < rateLimits_.size() ? rateLimits_[static_cast<std::size_t>(picked)] : display_.frameLimit;
        listFrameRates();
        pickFrameRate(limit);
    }
}

// The screen's rows show the frames a second they give on this screen. A fixed rate shows only when
// it is below the screen's and differs from the screen rows' (on a 120 Hz screen, 30 and 90).
void GraphicsOptionsPanel::listFrameRates() {
    std::vector<std::string> rows;
    rateLimits_.clear();
    std::vector<double> given;
    for (const ScreenRate& rate : kScreenRates) {
        rows.emplace_back(rate.text);
        if (refreshRate_ > 0) {
            given.push_back(refreshRate_ / rate.refreshes);
            rows.back() += std::format(" {} fps", std::lround(given.back()));
        }
        rateLimits_.push_back(rate.limit);
    }
    for (const int fixed : kFixedRates) {
        // Screens run a little off their round rates, as 119.88 Hz for 120.
        const auto same = [&](double fps) { return std::abs(fps - fixed) < 0.02 * fixed; };
        if (refreshRate_ > 0 && (fixed > refreshRate_ || std::any_of(given.begin(), given.end(), same))) {
            continue;
        }
        rows.push_back(std::format("{} fps", fixed));
        rateLimits_.push_back(fixed);
    }
    rows.emplace_back("Unlimited");
    rateLimits_.push_back(kNoLimit);
    frameRates_.setRows(rows);
}

// A saved fixed rate the list leaves out shows as the screen row giving the same, if one does.
void GraphicsOptionsPanel::pickFrameRate(int limit) {
    int row = -1;
    for (std::size_t i = 0; i < rateLimits_.size(); ++i) {
        if (rateLimits_[i] == limit) {
            row = static_cast<int>(i);
        }
    }
    for (std::size_t i = 0; row < 0 && limit > 0 && refreshRate_ > 0 && i < std::size(kScreenRates); ++i) {
        if (std::abs(refreshRate_ / kScreenRates[i].refreshes - limit) < 0.02 * limit) {
            row = static_cast<int>(i);
        }
    }
    frameRates_.pick(row);
}

void GraphicsOptionsPanel::showDisplay() {
    int size = -1;
    for (std::size_t i = 0; i < sizes_.size(); ++i) {
        if (sizes_[i][0] == display_.width && sizes_[i][1] == display_.height) {
            size = static_cast<int>(i);
        }
    }
    // A window too large for the screen's free area fits it instead, and the size asked for stays picked.
    for (std::size_t i = 0; size < 0 && i < sizes_.size(); ++i) {
        if (sizes_[i] == askedSize_) {
            size = static_cast<int>(i);
        }
    }
    // A saved size that isn't one of the screen's, such as a fitted window's, shows as the smallest that covers it.
    for (std::size_t i = 0; size < 0 && i < sizes_.size(); ++i) {
        if (sizes_[i][0] >= display_.width && sizes_[i][1] >= display_.height) {
            size = static_cast<int>(i);
        }
    }
    screenSizes_.pick(size);
    pickFrameRate(display_.frameLimit);
    checkFullscreen(display_.fullscreen);
    const int group = screen_.find(panel_, kSmoothEdgesGroup);
    // A saved level this card doesn't have shows as the one it gets.
    const int shownSamples = std::min(display_.smoothEdges, mostSmoothEdges_);
    int shownRow = 0;
    for (std::size_t i = 0; i < kSmoothEdgeSamples.size(); ++i) {
        if (kSmoothEdgeSamples[i] <= shownSamples) {
            shownRow = static_cast<int>(i);
        }
    }
    for (std::size_t i = 0; i < kSmoothEdgeSamples.size(); ++i) {
        const int row = screen_.find(group, kFirstSmoothEdgeRow + static_cast<std::uint32_t>(i));
        screen_.setEnabled(row, kSmoothEdgeSamples[i] <= mostSmoothEdges_);
        screen_.setChecked(row, static_cast<int>(i) == shownRow);
    }
    const int occlusion = screen_.find(panel_, kOcclusionGroup);
    for (int i = 0; i < static_cast<int>(ts2::content::kAmbientOcclusionStrengths.size()); ++i) {
        screen_.setChecked(screen_.find(occlusion, kFirstOcclusionRow + static_cast<std::uint32_t>(i)), i == display_.ambientOcclusion);
    }
    const int quality = screen_.find(panel_, kOcclusionSizeGroup);
    screen_.setChecked(screen_.find(quality, kOcclusionFullSize), !display_.occlusionHalfSize);
    screen_.setChecked(screen_.find(quality, kOcclusionHalfSize), display_.occlusionHalfSize);
    for (const auto& [id, realistic] : {std::pair{kWaterGroup, display_.realisticWater}, std::pair{kSkyGroup, display_.realisticSky}}) {
        const int water = screen_.find(panel_, id);
        screen_.setChecked(screen_.find(water, kClassicRow), !realistic);
        screen_.setChecked(screen_.find(water, kRealisticRow), realistic);
    }
    const int terrain = screen_.find(panel_, kTerrainGroup);
    for (std::size_t i = 0; i < std::size(kTerrainTiles); ++i) {
        screen_.setChecked(screen_.find(terrain, kFirstTerrainRow + static_cast<std::uint32_t>(i)), static_cast<int>(i) == display_.terrainDetail);
    }
    const int sizes = screen_.find(panel_, kUiSizeGroup);
    for (std::size_t i = 0; i < std::size(ts2::content::kUiScales); ++i) {
        screen_.setChecked(screen_.find(sizes, kFirstUiSizeRow + static_cast<std::uint32_t>(i)), ts2::content::kUiScales[i] == display_.uiScale);
    }
}

void GraphicsOptionsPanel::setMostSmoothEdges(int samples) {
    mostSmoothEdges_ = samples;
    if (panel_ >= 0) {
        showDisplay();
    }
}

GraphicsOptionsPanel::Display GraphicsOptionsPanel::pickedDisplay() const {
    Display out = display_;
    const int size = screenSizes_.picked();
    if (size >= 0 && static_cast<std::size_t>(size) < sizes_.size() && sizes_[static_cast<std::size_t>(size)] != askedSize_) {
        out.width = sizes_[static_cast<std::size_t>(size)][0];
        out.height = sizes_[static_cast<std::size_t>(size)][1];
    }
    const int rate = frameRates_.picked();
    if (rate >= 0 && static_cast<std::size_t>(rate) < rateLimits_.size()) {
        out.frameLimit = rateLimits_[static_cast<std::size_t>(rate)];
    }
    out.fullscreen = screen_.checked(screen_.find(panel_, kFullscreenOn));
    const int group = screen_.find(panel_, kSmoothEdgesGroup);
    for (std::size_t i = 0; i < kSmoothEdgeSamples.size(); ++i) {
        // Unless the player picks another, a level this card doesn't have stays saved for a card that does.
        if (screen_.checked(screen_.find(group, kFirstSmoothEdgeRow + static_cast<std::uint32_t>(i))) && kSmoothEdgeSamples[i] != std::min(display_.smoothEdges, mostSmoothEdges_)) {
            out.smoothEdges = kSmoothEdgeSamples[i];
        }
    }
    const int occlusion = screen_.find(panel_, kOcclusionGroup);
    for (int i = 0; i < static_cast<int>(ts2::content::kAmbientOcclusionStrengths.size()); ++i) {
        if (screen_.checked(screen_.find(occlusion, kFirstOcclusionRow + static_cast<std::uint32_t>(i)))) {
            out.ambientOcclusion = i;
        }
    }
    out.occlusionHalfSize = screen_.checked(screen_.find(screen_.find(panel_, kOcclusionSizeGroup), kOcclusionHalfSize));
    out.realisticWater = screen_.checked(screen_.find(screen_.find(panel_, kWaterGroup), kRealisticRow));
    out.realisticSky = screen_.checked(screen_.find(screen_.find(panel_, kSkyGroup), kRealisticRow));
    for (std::size_t i = 0; i < std::size(kTerrainTiles); ++i) {
        if (screen_.checked(screen_.find(screen_.find(panel_, kTerrainGroup), kFirstTerrainRow + static_cast<std::uint32_t>(i)))) {
            out.terrainDetail = static_cast<int>(i);
        }
    }
    const int sizes = screen_.find(panel_, kUiSizeGroup);
    for (std::size_t i = 0; i < std::size(ts2::content::kUiScales); ++i) {
        if (screen_.checked(screen_.find(sizes, kFirstUiSizeRow + static_cast<std::uint32_t>(i)))) {
            out.uiScale = ts2::content::kUiScales[i];
        }
    }
    return out;
}

void GraphicsOptionsPanel::setWindowSize(int width, int height) {
    display_.width = width;
    display_.height = height;
    if (panel_ >= 0 && shown()) {
        showDisplay();
    }
}

void GraphicsOptionsPanel::setUiScale(int percent) {
    display_.uiScale = percent;
    if (panel_ >= 0 && shown()) {
        showDisplay();
    }
}

bool GraphicsOptionsPanel::takeDisplayChanged() {
    const bool changed = displayChanged_;
    displayChanged_ = false;
    return changed;
}

void GraphicsOptionsPanel::centreBox() {
    const auto size = screen_.size();
    box_.centre(screen_, size[0], size[1]);
}

bool GraphicsOptionsPanel::mouseDown(int x, int y) {
    return panel_ >= 0 && shown() && !box_.isOpen() && (screenSizes_.mouseDown(x, y, now_) || frameRates_.mouseDown(x, y, now_));
}

void GraphicsOptionsPanel::mouseMove(int x, int y) {
    screenSizes_.mouseMove(x, y);
    frameRates_.mouseMove(x, y);
}

void GraphicsOptionsPanel::mouseUp(int x, int y) {
    screenSizes_.mouseUp(x, y);
    frameRates_.mouseUp(x, y);
}

bool GraphicsOptionsPanel::wheel(int x, int y, int delta) {
    return panel_ >= 0 && shown() && !box_.isOpen() && (screenSizes_.wheel(x, y, delta) || frameRates_.wheel(x, y, delta));
}

void GraphicsOptionsPanel::showChoices() {
    if (panel_ < 0) {
        return;
    }
    for (const GraphicsGroup& group : kGraphicsGroups) {
        const int window = screen_.find(panel_, group.window);
        const std::uint32_t checked = ts2::content::buttonForSetting(group, setting(group));
        for (int i = 0; i < group.count; ++i) {
            const std::uint32_t button = group.choices[static_cast<std::size_t>(i)].button;
            screen_.setChecked(screen_.find(window, button), button == checked);
        }
    }
    showDisplay();
}

void GraphicsOptionsPanel::pick(int group, int button) {
    for (const int other : screen_.children(group)) {
        if (screen_.id(other) != 0) {
            screen_.setChecked(other, other == button);
        }
    }
}

void GraphicsOptionsPanel::pickDefaults() {
    // The game's button leaves the lists as they are.
    for (const GraphicsGroup& group : kGraphicsGroups) {
        const int window = screen_.find(panel_, group.window);
        const std::uint32_t checked = ts2::content::buttonForSetting(group, group.highLevel);
        for (int i = 0; i < group.count; ++i) {
            const std::uint32_t button = group.choices[static_cast<std::size_t>(i)].button;
            screen_.setChecked(screen_.find(window, button), button == checked);
        }
    }
}

int GraphicsOptionsPanel::checkedSetting(const GraphicsGroup& group) const {
    const int window = screen_.find(panel_, group.window);
    for (int i = 0; i < group.count; ++i) {
        const auto& choice = group.choices[static_cast<std::size_t>(i)];
        if (screen_.checked(screen_.find(window, choice.button))) {
            return choice.setting;
        }
    }
    return -1;
}

void GraphicsOptionsPanel::apply() {
    auto next = values_;
    bool any = false;
    for (const GraphicsGroup& group : kGraphicsGroups) {
        const int picked = checkedSetting(group);
        if (picked >= 0 && picked != setting(group)) {
            next[savedKey(group)] = std::to_string(picked);
            any = true;
        }
    }
    const Display picked = pickedDisplay();
    if (!any && picked == display_) {
        return;
    }
    before_ = values_;
    values_ = std::move(next);
    changed_ = true;
    displayBefore_ = display_;
    askedBefore_ = askedSize_;
    if (const int size = screenSizes_.picked(); size >= 0 && static_cast<std::size_t>(size) < sizes_.size()) {
        askedSize_ = sizes_[static_cast<std::size_t>(size)];
    }
    if (picked != display_) {
        display_ = picked;
        displayChanged_ = true;
    }
    countdown_ = kCountdown;
    nextCount_ = now_ + kCountMs;
    const auto size = screen_.size();
    box_.open(screen_, size[0], size[1], boxTitle_, std::format("{}\n{}", boxText_, countdown_), {{kYes, yes_}, {kNo, no_}});
}

void GraphicsOptionsPanel::keep() {
    box_.close(screen_);
    std::map<std::string, std::pair<std::string, bool>> settings;
    for (const auto& [key, value] : values_) {
        const auto was = before_.find(key);
        if (was == before_.end() || was->second != value) {
            settings[key] = {value, false};
        }
    }
    if (!settings.empty() && !ts2::content::saveUserProps(userFolder_, settings)) {
        ts2::diag::warnOnce("graphics options: the choices could not be saved to userProps.xml");
    }
    // The size goes with the game's own settings, the rest with NeoTS2's. A size is saved only when
    // picked, as a full screen's or a fitted window's real size isn't a choice.
    if (display_.width != displayBefore_.width || display_.height != displayBefore_.height) {
        if (!display_.fullscreen && !ts2::content::saveUserProps(userFolder_, {{kDisplayModeKey, {std::format("{}x{}x32x0", display_.width, display_.height), false}}})) {
            ts2::diag::warnOnce("graphics options: the screen size could not be saved to userProps.xml");
        }
    }
    if (display_.fullscreen != displayBefore_.fullscreen || display_.frameLimit != displayBefore_.frameLimit || display_.smoothEdges != displayBefore_.smoothEdges ||
        display_.ambientOcclusion != displayBefore_.ambientOcclusion || display_.occlusionHalfSize != displayBefore_.occlusionHalfSize ||
        display_.uiScale != displayBefore_.uiScale || display_.realisticWater != displayBefore_.realisticWater ||
        display_.realisticSky != displayBefore_.realisticSky ||
        display_.terrainDetail != displayBefore_.terrainDetail) {
        if (!ts2::content::saveNeoProps(userFolder_, {{kFullscreenKey, {display_.fullscreen ? "true" : "false", true}},
                                                      {kFrameLimitKey, {std::to_string(display_.frameLimit), false}},
                                                      {kSmoothEdgesKey, {std::to_string(display_.smoothEdges), false}},
                                                      {kOcclusionKey, {std::to_string(display_.ambientOcclusion), false}},
                                                      {kOcclusionSizeKey, {display_.occlusionHalfSize ? "true" : "false", true}},
                                                      {kUiScaleKey, {std::to_string(display_.uiScale), false}},
                                                      {kWaterKey, {display_.realisticWater ? "true" : "false", true}},
                                                      {kSkyKey, {display_.realisticSky ? "true" : "false", true}},
                                                      {kTerrainKey, {std::to_string(display_.terrainDetail), false}}})) {
            ts2::diag::warnOnce("graphics options: the display choices could not be saved");
        }
    }
}

void GraphicsOptionsPanel::putBack() {
    box_.close(screen_);
    values_ = before_;
    changed_ = true;
    askedSize_ = askedBefore_;
    if (display_ != displayBefore_) {
        display_ = displayBefore_;
        displayChanged_ = true;
    }
    showChoices();
}

void GraphicsOptionsPanel::tick(std::uint64_t ms) {
    now_ = ms;
    screenSizes_.tick(ms);
    frameRates_.tick(ms);
    if (!box_.isOpen() || ms < nextCount_) {
        return;
    }
    nextCount_ = ms + kCountMs;
    if (--countdown_ <= 0) {
        putBack();
    } else {
        box_.setBody(screen_, std::format("{}\n{}", boxText_, countdown_));
    }
}

bool GraphicsOptionsPanel::clicked(int window) {
    if (panel_ < 0 || window < 0) {
        return false;
    }
    if (box_.isOpen()) {
        const auto answer = box_.clicked(screen_, window);
        if (!answer) {
            return false;
        }
        if (*answer == kYes) {
            keep();
        } else {
            putBack();
        }
        return true;
    }
    // The button opens its panel, or closes it when it is the one open, as each panel button does.
    if (window == menu_.button(kMenuButton)) {
        show(!shown());
        return true;
    }
    bool inPanel = false;
    for (int w = window; w >= 0; w = screen_.parent(w)) {
        inPanel = inPanel || w == panel_;
    }
    if (!inPanel) {
        return false;
    }
    const std::uint32_t id = screen_.id(window);
    if (id == kApply) {
        apply();
    } else if (id == kDefaultSettings) {
        pickDefaults();
    } else if (id == kFullscreenOff || id == kFullscreenOn) {
        checkFullscreen(id == kFullscreenOn);
    } else if (screen_.parent(window) == screen_.find(panel_, kSmoothEdgesGroup) && id >= kFirstSmoothEdgeRow && id < kFirstSmoothEdgeRow + kSmoothEdgeSamples.size()) {
        for (std::size_t i = 0; i < kSmoothEdgeSamples.size(); ++i) {
            const int row = screen_.find(screen_.parent(window), kFirstSmoothEdgeRow + static_cast<std::uint32_t>(i));
            screen_.setChecked(row, row == window);
        }
    } else if (id != 0 && ownGroup(screen_.parent(window))) {
        // One choice in each of NeoTS2's own groups.
        for (const int row : screen_.children(screen_.parent(window))) {
            if (screen_.id(row) != 0) {
                screen_.setChecked(row, row == window);
            }
        }
    } else if (!screenSizes_.clicked(window) && !frameRates_.clicked(window)) {
        for (const GraphicsGroup& group : kGraphicsGroups) {
            if (screen_.parent(window) != screen_.find(panel_, group.window)) {
                continue;
            }
            for (int i = 0; i < group.count; ++i) {
                if (group.choices[static_cast<std::size_t>(i)].button == id) {
                    pick(screen_.parent(window), window);
                }
            }
        }
    }
    return true;
}

bool GraphicsOptionsPanel::takeChanged() {
    const bool changed = changed_;
    changed_ = false;
    return changed;
}

} // namespace app
