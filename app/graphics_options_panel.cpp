#include "graphics_options_panel.hpp"

#include "content/user_props.hpp"
#include "diag/log.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <charconv>
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
struct FrameRate {
    const char* text;
    int limit;
};
constexpr FrameRate kFrameRates[] = {{"Screen", GraphicsOptionsPanel::kWaitForScreen}, {"30", 30}, {"60", 60}, {"120", 120}, {"Unlimited", GraphicsOptionsPanel::kNoLimit}};
// The Shadows group's title. The game greys it with its buttons.
constexpr std::uint32_t kShadowsTitle = 0x94;
constexpr std::uint32_t kFrameRateTitle = 0xAD7FAA35;
// Smooth Edges. The game's slider runs over the levels a card has, so one place on it means a different
// level on each card. NeoTS2 names the levels with radio buttons in its place, in the panel's other
// groups' rows, and keeps the choice as its own setting. Ambient Occlusion, not in the original game,
// shares the group.
constexpr std::uint32_t kSmoothEdgesGroup = 0xC0, kSmoothEdgesBar = 0xECD97AFE, kSmoothEdgesLess = 0x2E10C53D, kSmoothEdgesMore = 0x2E10C541;
constexpr std::uint32_t kFirstSmoothEdgeRow = 0xC8;
constexpr std::uint32_t kFirstOcclusionRow = 0xCC;
// Ambient Occlusion's Full Size and Half Size, beside its levels.
constexpr std::uint32_t kOcclusionFullSize = 0xD0, kOcclusionHalfSize = 0xD1;
constexpr int kOcclusionSizeLeft = 80;
constexpr int kSmoothEdgeRowTop = 15, kOcclusionTitleTop = 86, kOcclusionRowTop = 110, kRowHeight = 16, kRowWidth = 83;
constexpr int kGroupWidth = 163, kGroupHeight = 180;
// How much taller the panel is than its script. It is four rows of the Screen Size list, which gains them.
constexpr int kExtraHeight = 68;
// What sits lower than this in the panel's script moves down, and the lists' parts start at the list top.
constexpr int kLowerGroupsTop = 300, kListTop = 17;
// The panel's picture, stretched only in a plain band of it, so its corners and title keep their shape.
constexpr std::uint32_t kPictureWidth = 681, kPictureHeight = 430, kBandTop = 300, kBandBottom = 304;
const std::string kPanelPicture = "{499db772,dafe9000}";
constexpr std::uint32_t kOffText = 0x00800003;  // string id, leakcheck: ok
constexpr std::uint32_t kMediumText = 0x00800005;  // string id, leakcheck: ok
// UI Size, not in the original game, in the free room under the Frame Rate list: the sizes in two
// columns of rows, the first three on the left.
constexpr std::uint32_t kUiSizeGroup = 0xD7, kFirstUiSizeRow = 0xD8;
constexpr int kUiSizeGap = 4, kUiSizeHeight = 72, kUiSizeColumn = 3;
const std::string kUiScaleKey = "uiScale";
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
        if (std::from_chars(saved->second.data(), end, limit).ptr == end &&
            std::any_of(std::begin(kFrameRates), std::end(kFrameRates), [&](const FrameRate& rate) { return rate.limit == limit; })) {
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
    makeTaller();
    screenSizes_.attach(screen_, screen_.find(panel_, kScreenSizeList));
    frameRates_.attach(screen_, screen_.find(panel_, kFrameRateList));
    std::vector<std::string> rates;
    for (const FrameRate& rate : kFrameRates) {
        rates.emplace_back(rate.text);
    }
    frameRates_.setRows(rates);
    listScreenSizes();
    addSmoothEdgeRows();
    addUiSizeRows();
    for (const GraphicsGroup& group : kGraphicsGroups) {
        const int window = screen_.find(panel_, group.window);
        const bool bumps = group.option == "MaterialDetail";
        for (int i = 0; !worksInFull(group.option) && i < group.count; ++i) {
            screen_.setTip(screen_.find(window, group.choices[static_cast<std::size_t>(i)].button), bumps ? kOnlyBumps : kNotWorking);
        }
    }
    showChoices();
}

void GraphicsOptionsPanel::makeTaller() {
    const std::vector<int> parts = screen_.children(panel_);
    if (parts.size() < 2) {
        return;
    }
    const int picture = parts[0], above = parts[1];
    for (const int part : parts) {
        if (screen_.y(part) > kLowerGroupsTop) {
            screen_.move(part, screen_.x(part), screen_.y(part) + kExtraHeight);
        }
    }
    screen_.resize(panel_, screen_.width(panel_), screen_.height(panel_) + kExtraHeight);
    // The picture in three: the top as it is, the band stretched over the extra height, the bottom as it is.
    const int width = static_cast<int>(kPictureWidth);
    const auto addPart = [&](int top, int height, int partTop, int partBottom) {
        const ts2::ui::Window window{{{"iid", "IGZWinBMP"}, {"area", std::format("(0,{},{},{})", top, width, top + height)}, {"winflag_visible", "yes"}, {"winflag_mousetrans", "yes"},
                                      {"winflag_alphablend", "yes"}, {"image", kPanelPicture}},
                                     {}};
        const int made = screen_.addWindows({window}, 0, top, panel_);
        screen_.setImagePart(made, {0, partTop, static_cast<int>(kPictureWidth), partBottom});
        screen_.placeUnder(made, above);
    };
    addPart(kBandTop, kBandBottom - kBandTop + kExtraHeight, kBandTop, kBandBottom);
    addPart(kBandBottom + kExtraHeight, kPictureHeight - kBandBottom, kBandBottom, kPictureHeight);
    screen_.resize(picture, width, kBandTop);
    screen_.setImagePart(picture, {0, 0, static_cast<int>(kPictureWidth), kBandTop});
    // The Screen Size list and its background grow.
    const int group = screen_.parent(screen_.find(panel_, kScreenSizeList));
    screen_.resize(group, screen_.width(group), screen_.height(group) + kExtraHeight);
    for (const int part : screen_.children(group)) {
        if (screen_.y(part) == kListTop) {
            screen_.resize(part, screen_.width(part), screen_.height(part) + kExtraHeight);
        }
    }
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
    const ts2::ui::Window window{{{"iid", "IGZWinGen"}, {"id", std::format("0x{:08x}", kUiSizeGroup)}, {"winflag_visible", "yes"},
                                  {"area", std::format("(0,0,{},{})", kGroupWidth, kUiSizeHeight)}},
                                 {}};
    const int group = screen_.addWindows({window}, left, top + kUiSizeGap, panel_);
    addGroupTitle(group, 0, "UI Size");
    for (std::size_t i = 0; i < std::size(ts2::content::kUiScales); ++i) {
        const int column = static_cast<int>(i) / kUiSizeColumn, row = static_cast<int>(i) % kUiSizeColumn;
        addRadioRow(group, kFirstUiSizeRow + static_cast<std::uint32_t>(i), kOcclusionRowTop - kOcclusionTitleTop + row * kRowHeight,
                    std::format("{}%", ts2::content::kUiScales[i]), column * kOcclusionSizeLeft);
    }
}

void GraphicsOptionsPanel::addSmoothEdgeRows() {
    const int group = screen_.find(panel_, kSmoothEdgesGroup);
    for (const std::uint32_t part : {kSmoothEdgesBar, kSmoothEdgesLess, kSmoothEdgesMore}) {
        screen_.setVisible(screen_.find(group, part), false);
    }
    screen_.resize(group, kGroupWidth, kGroupHeight);
    const std::string off = screen_.data().text(kOptionsText, kOffText);
    for (std::size_t i = 0; i < kSmoothEdgeSamples.size(); ++i) {
        addRadioRow(group, kFirstSmoothEdgeRow + static_cast<std::uint32_t>(i), kSmoothEdgeRowTop + static_cast<int>(i) * kRowHeight, i == 0 ? off : std::format("{}x", kSmoothEdgeSamples[i]));
    }
    addGroupTitle(group, kOcclusionTitleTop, "Ambient Occlusion");
    const std::string levels[] = {off, "Light", screen_.data().text(kOptionsText, kMediumText), "Dark"};
    for (std::size_t i = 0; i < std::size(levels); ++i) {
        addRadioRow(group, kFirstOcclusionRow + static_cast<std::uint32_t>(i), kOcclusionRowTop + static_cast<int>(i) * kRowHeight, levels[i]);
    }
    addRadioRow(group, kOcclusionFullSize, kOcclusionRowTop, "Full Size", kOcclusionSizeLeft);
    addRadioRow(group, kOcclusionHalfSize, kOcclusionRowTop + kRowHeight, "Half Size", kOcclusionSizeLeft);
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

void GraphicsOptionsPanel::showDisplay() {
    int size = -1;
    for (std::size_t i = 0; i < sizes_.size(); ++i) {
        if (sizes_[i][0] == display_.width && sizes_[i][1] == display_.height) {
            size = static_cast<int>(i);
        }
    }
    screenSizes_.pick(size);
    int rate = 0;
    for (std::size_t i = 0; i < std::size(kFrameRates); ++i) {
        if (kFrameRates[i].limit == display_.frameLimit) {
            rate = static_cast<int>(i);
        }
    }
    frameRates_.pick(rate);
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
    for (int i = 0; i < static_cast<int>(ts2::content::kAmbientOcclusionStrengths.size()); ++i) {
        screen_.setChecked(screen_.find(group, kFirstOcclusionRow + static_cast<std::uint32_t>(i)), i == display_.ambientOcclusion);
    }
    screen_.setChecked(screen_.find(group, kOcclusionFullSize), !display_.occlusionHalfSize);
    screen_.setChecked(screen_.find(group, kOcclusionHalfSize), display_.occlusionHalfSize);
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
    if (size >= 0 && static_cast<std::size_t>(size) < sizes_.size()) {
        out.width = sizes_[static_cast<std::size_t>(size)][0];
        out.height = sizes_[static_cast<std::size_t>(size)][1];
    }
    const int rate = frameRates_.picked();
    if (rate >= 0 && static_cast<std::size_t>(rate) < std::size(kFrameRates)) {
        out.frameLimit = kFrameRates[static_cast<std::size_t>(rate)].limit;
    }
    out.fullscreen = screen_.checked(screen_.find(panel_, kFullscreenOn));
    const int group = screen_.find(panel_, kSmoothEdgesGroup);
    for (std::size_t i = 0; i < kSmoothEdgeSamples.size(); ++i) {
        // Unless the player picks another, a level this card doesn't have stays saved for a card that does.
        if (screen_.checked(screen_.find(group, kFirstSmoothEdgeRow + static_cast<std::uint32_t>(i))) && kSmoothEdgeSamples[i] != std::min(display_.smoothEdges, mostSmoothEdges_)) {
            out.smoothEdges = kSmoothEdgeSamples[i];
        }
    }
    for (int i = 0; i < static_cast<int>(ts2::content::kAmbientOcclusionStrengths.size()); ++i) {
        if (screen_.checked(screen_.find(group, kFirstOcclusionRow + static_cast<std::uint32_t>(i)))) {
            out.ambientOcclusion = i;
        }
    }
    out.occlusionHalfSize = screen_.checked(screen_.find(group, kOcclusionHalfSize));
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
    return panel_ >= 0 && shown() && !box_.isOpen() && (screenSizes_.mouseDown(x, y) || frameRates_.mouseDown(x, y));
}

void GraphicsOptionsPanel::mouseMove(int y) {
    if (screenSizes_.holdsMouse()) {
        screenSizes_.mouseMove(y);
    }
    if (frameRates_.holdsMouse()) {
        frameRates_.mouseMove(y);
    }
}

void GraphicsOptionsPanel::mouseUp() {
    screenSizes_.mouseUp();
    frameRates_.mouseUp();
}

bool GraphicsOptionsPanel::wheel(int x, int y, int rows) {
    return panel_ >= 0 && shown() && !box_.isOpen() && (screenSizes_.wheel(x, y, rows) || frameRates_.wheel(x, y, rows));
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
        display_.uiScale != displayBefore_.uiScale) {
        if (!ts2::content::saveNeoProps(userFolder_, {{kFullscreenKey, {display_.fullscreen ? "true" : "false", true}},
                                                      {kFrameLimitKey, {std::to_string(display_.frameLimit), false}},
                                                      {kSmoothEdgesKey, {std::to_string(display_.smoothEdges), false}},
                                                      {kOcclusionKey, {std::to_string(display_.ambientOcclusion), false}},
                                                      {kOcclusionSizeKey, {display_.occlusionHalfSize ? "true" : "false", true}},
                                                      {kUiScaleKey, {std::to_string(display_.uiScale), false}}})) {
            ts2::diag::warnOnce("graphics options: the display choices could not be saved");
        }
    }
}

void GraphicsOptionsPanel::putBack() {
    box_.close(screen_);
    values_ = before_;
    changed_ = true;
    if (display_ != displayBefore_) {
        display_ = displayBefore_;
        displayChanged_ = true;
    }
    showChoices();
}

void GraphicsOptionsPanel::tick(std::uint64_t ms) {
    now_ = ms;
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
    } else if (screen_.parent(window) == screen_.find(panel_, kSmoothEdgesGroup) && id >= kFirstOcclusionRow && id < kFirstOcclusionRow + ts2::content::kAmbientOcclusionStrengths.size()) {
        for (std::size_t i = 0; i < ts2::content::kAmbientOcclusionStrengths.size(); ++i) {
            const int row = screen_.find(screen_.parent(window), kFirstOcclusionRow + static_cast<std::uint32_t>(i));
            screen_.setChecked(row, row == window);
        }
    } else if (screen_.parent(window) == screen_.find(panel_, kSmoothEdgesGroup) && (id == kOcclusionFullSize || id == kOcclusionHalfSize)) {
        screen_.setChecked(screen_.find(screen_.parent(window), kOcclusionFullSize), id == kOcclusionFullSize);
        screen_.setChecked(screen_.find(screen_.parent(window), kOcclusionHalfSize), id == kOcclusionHalfSize);
    } else if (screen_.parent(window) == screen_.find(panel_, kUiSizeGroup) && id >= kFirstUiSizeRow && id < kFirstUiSizeRow + std::size(ts2::content::kUiScales)) {
        for (std::size_t i = 0; i < std::size(ts2::content::kUiScales); ++i) {
            const int row = screen_.find(screen_.parent(window), kFirstUiSizeRow + static_cast<std::uint32_t>(i));
            screen_.setChecked(row, row == window);
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
