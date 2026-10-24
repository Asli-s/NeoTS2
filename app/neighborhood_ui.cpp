#include "neighborhood_ui.hpp"

#include "neighborhood.hpp"

#include "content/neighborhood.hpp"
#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/fami.hpp"
#include "formats/image.hpp"
#include "formats/lot_description.hpp"
#include "formats/str.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <map>
#include <utility>

namespace app {

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kScripts = 0xA99D8A11;
// The panel (the puck, the top bar) and its windows by id.
constexpr std::uint32_t kPanel = 0x49000000, kMainMenu = 0x00000002, kCityName = 0x00000003;
constexpr std::uint32_t kPuck = 0x4be6ed7d, kTopBar = 0x4be6ed7e, kTopBarBackground = 0x00000001;
constexpr std::uint32_t kPuckFamilies = 0x2be6f563, kPuckLots = 0x4be6f571, kPuckOptions = 0x2bf08bec;
// The neighborhood's own options menu and its Main Menu and Quit buttons. A hidden 1x1 window in it
// is its anchor, whose caption names the puck window it goes on ("aabbccdd:tl").
constexpr std::uint32_t kOptionsMenu = 0x49001028, kOptionsMainMenu = 0x0000a221, kOptionsQuit = 0x0000a220;
// The top bar's background ends this far past the neighborhood's name (its text's left edge, 2 in
// from the name's window, plus its width).
constexpr int kNameToBarEnd = 2 + 23;
// The top bar's choosers of a neighborhood's sub-neighborhoods, with the name its files carry
// (<hood>_<kind>NNN.package): each has a button for when one is attached and one (a plus) for
// when none is.
constexpr std::pair<std::uint32_t, const char*> kSubNeighborhoods[] = {
    {0x00000008, "Vacation"}, {0x10000003, "Suburb"}, {0x00000006, "Downtown"}, {0x00000004, "University"}};

// The bins: families without a home, and the lots to place. Each opens above the puck, 112 to its
// right and 33 above its top.
constexpr std::uint32_t kFamilyBin = 0x49001004, kLotBin = 0x49001003;
constexpr int kBinX = 112, kBinY = -33;
// Family bin windows.
constexpr std::uint32_t kFamilyStripHolder = 0x0000000b, kTallStrip = 0x00002000, kMoveFamily = 0x6d13dda1, kNewFamily = 0x0000000a,
                        kToCollege = 0x8ea600a9;
// Lot bin windows: the backgrounds and parts of each view (all lots, empty lots, specialty lots),
// its view buttons, and the filter of the lots view (occupied, unoccupied, all).
constexpr std::uint32_t kLotsBackground = 0x0000c200, kEmptyLotsBackground = 0x0000d200, kLotsView = 0x0000c000,
                        kEmptyLotsView = 0x0000d000, kEmptyLotsSizes = 0x1111d000, kSpecialtyView = 0x0000f000,
                        kLotsButton = 0x0000c100, kEmptyLotsButton = 0x0000d100, kAllLots = 0x0000c600;
// A strip of tiles: its row, the row's parts, and the tile each item shows in.
constexpr std::uint32_t kStripRow = 0x00001000, kStripLeftCap = 0x00001001, kStripMiddle = 0x00001002, kStripRightCap = 0x00001003,
                        kStripBack = 0x00001004, kStripNext = 0x00001005, kStripArea = 0x00001006, kStripScroll = 0x00001008,
                        kStripTaller = 0x00001009, kStripCorner = 0x0000100a;
constexpr std::uint32_t kTile = 0x49001006, kTilePicture = 0x00000002;
// Tiles start 52 into the row, 95 apart. The row ends 51 past the last. A row holds at least 4 and
// as many as fit before 63 from the screen's right edge.
constexpr int kTileLeft = 52, kTileStep = 95, kTileTop = 9, kRowTail = 51, kMinTiles = 4, kScreenRightGap = 63;
// The lot catalog's picture of a lot.
constexpr std::uint32_t kImageType = 0x856DDBAC, kLotPicture = 0x35CA0002;
constexpr std::uint32_t kFamilyType = 0x46414D49, kFamilyPictureType = 0x8C3CE95A;
// Families from this id up are the game's own (townies, service people), not households.
constexpr std::uint32_t kFirstGameFamily = 0x7FE0;
constexpr std::uint32_t kStringsType = 0x53545223;
// The lot card (Lot Info Dialog) and the layouts used here: a household's lot with a bio, and a
// community lot. Each layout's windows share its high digits.
constexpr std::uint32_t kLotCard = 0x49001000;
constexpr std::uint32_t kFamilyLayout = 0xa000, kFamilyPicture = 0xa005, kFamilyName = 0xa006, kFamilyAddress = 0xa007,
                        kFamilyFunds = 0xa008, kFamilyBio = 0xa009, kFamilyEnter = 0xa002, kFamilyClose = 0xa00a;
constexpr std::uint32_t kCommunityLayout = 0xe000, kCommunityAddress = 0xe003, kCommunityText = 0xe004, kCommunityClose = 0xe007;
// The card sits this far beside its lot, on the side away from the screen's middle.
constexpr int kCardGap = 60;
// The tag over the lot under the mouse: a soft white box stretched around its content, 16 pixels
// round it. It holds the sun icon on lots people live on or can buy, and the name in GenHeader, black.
constexpr std::uint32_t kTagImageGroup = 0x499db772, kTagBox = 0x14416193, kTagSun = 0xdafe8501;
// The box is the top left 60 x 60 of its 75 x 75 image. The rest is empty.
constexpr std::uint32_t kTagBoxSize = 60;
constexpr int kTagAbove = 28;
constexpr int kTagPadding = 16, kTagIcon = 24, kTagTextGap = 4;  // the name has 4 pixels on each side
const std::string kTagFont = "0x4a809916";  // GenHeader
// "For Sale" (Neighborhood text 0x82, string 1).
constexpr std::uint32_t kNeighborhoodText = 0x7F3A262A, kForSale = 0x00820001;  // a string id, leakcheck: ok
// Lot types that are community lots.
constexpr std::uint8_t kCommunityTypes[] = {1, 7, 10};

// Of the windows in `window` that share an id (versions of one button), shows only the one the game
// shows. That is the first not hidden in the script, but a sub-neighborhood chooser's second (its
// plus) when the neighborhood has none of that kind attached. Of those in `pets` it is the Pets
// version (the first).
void chooseVersions(ts2::ui::Screen& screen, int window, const fs::path& folder, const std::vector<std::uint32_t>& pets = {}) {
    std::map<std::uint32_t, std::vector<int>> byId;
    for (const int c : screen.children(window)) {
        if (screen.id(c) != 0) {
            byId[screen.id(c)].push_back(c);
        }
    }
    const std::string hood = folder.filename().string();
    for (const auto& [id, versions] : byId) {
        if (versions.size() < 2) {
            continue;
        }
        if (std::find(pets.begin(), pets.end(), id) != pets.end()) {
            for (std::size_t i = 0; i < versions.size(); ++i) {
                screen.setVisible(versions[i], i == 0);
            }
            continue;
        }
        std::vector<int> shown;
        for (const int v : versions) {
            if (screen.visible(v)) {
                shown.push_back(v);
            }
        }
        std::size_t pick = 0;
        for (const auto& [chooser, kind] : kSubNeighborhoods) {
            if (chooser != id || shown.size() < 2) {
                continue;
            }
            bool attached = false;
            std::error_code ec;
            for (fs::directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
                const std::string name = it->path().filename().string();
                attached = attached || (name.starts_with(hood + "_" + kind) && it->path().extension() == ".package");
            }
            pick = attached ? 0 : 1;
        }
        for (std::size_t i = 0; i < shown.size(); ++i) {
            screen.setVisible(shown[i], i == pick);
        }
    }
}

// The windows directly in `window` with an id.
// Where a window's top left is on the screen: its place in each of its parents added up.
std::array<int, 2> onScreen(const ts2::ui::Screen& screen, int window) {
    std::array<int, 2> at{0, 0};
    for (int w = window; w >= 0; w = screen.parent(w)) {
        at[0] += screen.x(w);
        at[1] += screen.y(w);
    }
    return at;
}

// A panel's anchor: the window inside it whose caption is a puck window's id and a corner
// (":tl" or ":bl"), or -1.
int anchorMark(const ts2::ui::Screen& screen, int window) {
    for (const int c : screen.children(window)) {
        const std::string& caption = screen.text(c);
        if (caption.size() > 3 && (caption.ends_with(":tl") || caption.ends_with(":bl"))) {
            return c;
        }
        if (const int found = anchorMark(screen, c); found >= 0) {
            return found;
        }
    }
    return -1;
}

std::vector<int> childrenWithId(const ts2::ui::Screen& screen, int window, std::uint32_t id) {
    std::vector<int> out;
    for (const int c : screen.children(window)) {
        if (screen.id(c) == id) {
            out.push_back(c);
        }
    }
    return out;
}

// The households without a home, in id order, as their pictures (Thumbnails/<hood>_FamilyThumbnails).
std::vector<ts2::tga::Pixels> homelessFamilies(const fs::path& folder) {
    std::vector<std::uint32_t> ids;
    try {
        ts2::dbpf::Package package{ts2::content::neighborhoodPackage(folder)};
        for (const auto& e : package.entries()) {
            if (e.key.type != kFamilyType || e.key.instance >= kFirstGameFamily) {
                continue;
            }
            const auto family = ts2::fami::parse(package.read(e));
            if (family.lot == 0 && !family.members.empty()) {
                ids.push_back(e.key.instance);
            }
        }
    } catch (const std::exception&) {
        return {};
    }
    std::sort(ids.begin(), ids.end());
    std::vector<ts2::tga::Pixels> out(ids.size());
    const fs::path pictures = folder / "Thumbnails" / (folder.filename().u8string() + u8"_FamilyThumbnails.package");
    if (fs::is_regular_file(pictures)) {
        ts2::dbpf::Package package{pictures};
        for (const auto& e : package.entries()) {
            const auto at = std::find(ids.begin(), ids.end(), e.key.instance);
            if (e.key.type == kFamilyPictureType && at != ids.end()) {
                if (auto picture = ts2::image::decode(package.read(e))) {
                    out[static_cast<std::size_t>(at - ids.begin())] = std::move(*picture);
                }
            }
        }
    }
    return out;
}

// The cards of the lots a household lives on and of the community lots. A household's name is
// string 0 of its string table (instance = family id), its bio string 1.
std::map<std::uint32_t, NeighborhoodUi::LotCard> lotCards(const fs::path& folder) {
    std::map<std::uint32_t, NeighborhoodUi::LotCard> cards;
    std::map<std::uint32_t, std::uint32_t> familyOnLot;  // family id by lot
    try {
        ts2::dbpf::Package package{ts2::content::neighborhoodPackage(folder)};
        std::map<std::uint32_t, ts2::nhood::LotDescription> lots;
        std::map<std::uint32_t, ts2::fami::Family> families;
        std::map<std::uint32_t, ts2::str::StringTable> strings;
        for (const auto& e : package.entries()) {
            if (e.key.type == ts2::nhood::kLotDescriptionType) {
                lots[e.key.instance] = ts2::nhood::parseLotDescription(package.read(e));
            } else if (e.key.type == kFamilyType && e.key.instance < kFirstGameFamily) {
                families[e.key.instance] = ts2::fami::parse(package.read(e));
            } else if (e.key.type == kStringsType && e.key.instance < kFirstGameFamily) {
                strings[e.key.instance] = ts2::str::parse(package.read(e));
            }
        }
        for (const auto& [id, lot] : lots) {
            if (std::find(std::begin(kCommunityTypes), std::end(kCommunityTypes), lot.type) != std::end(kCommunityTypes)) {
                cards[id] = {true, lot.name, {}, lot.description, {}};
            } else {
                cards[id] = {false, lot.name, {}, {}, {}, true};  // for sale, unless a household lives there
            }
        }
        for (const auto& [id, family] : families) {
            const auto lot = lots.find(static_cast<std::uint32_t>(family.lot));
            if (family.lot <= 0 || family.members.empty() || lot == lots.end()) {
                continue;
            }
            NeighborhoodUi::LotCard card{false, lot->second.name, {}, {}, {}};
            if (const auto table = strings.find(id); table != strings.end()) {
                const auto texts = table->second.forLanguage(1);
                card.name = texts.size() > 0 ? texts[0]->value : std::string{};
                card.text = texts.size() > 1 ? texts[1]->value : std::string{};
            }
            cards[lot->first] = std::move(card);
            familyOnLot[lot->first] = id;
        }
    } catch (const std::exception& ex) {
        ts2::diag::warnOnce(std::string("neighborhood: the lots' cards can't be read: ") + ex.what());
        return {};
    }
    const fs::path pictures = folder / "Thumbnails" / (folder.filename().u8string() + u8"_FamilyThumbnails.package");
    if (fs::is_regular_file(pictures)) {
        try {
            ts2::dbpf::Package package{pictures};
            for (const auto& e : package.entries()) {
                for (const auto& [lot, family] : familyOnLot) {
                    if (e.key.type == kFamilyPictureType && e.key.instance == family) {
                        if (auto picture = ts2::image::decode(package.read(e))) {
                            cards[lot].picture = std::move(*picture);
                        }
                    }
                }
            }
        } catch (const std::exception& ex) {
            ts2::diag::warnOnce(std::string("neighborhood: the family pictures can't be read: ") + ex.what());
        }
    }
    return cards;
}

// A window of the lot tag, placed by showTag.
ts2::ui::Window tagWindow(const std::string& kind, std::vector<std::pair<std::string, std::string>> more) {
    ts2::ui::Window window;
    window.attributes = {{"iid", kind}, {"paint", "yes"}, {"area", "(0,0,1,1)"}, {"winflag_visible", "yes"}};
    window.attributes.insert(window.attributes.end(), more.begin(), more.end());
    return window;
}

// The lot catalog (LotCatalog/cx_NNNNNNNN.package, newest first), as each lot's picture.
std::vector<ts2::tga::Pixels> catalogLots(const fs::path& user) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(user / "LotCatalog", ec), end; it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.starts_with("cx_") && name.find("Character") == std::string::npos && it->path().extension() == ".package") {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return a.filename() > b.filename(); });
    std::vector<ts2::tga::Pixels> out;
    for (const auto& file : files) {
        ts2::tga::Pixels picture;
        try {
            ts2::dbpf::Package package{file};
            for (const auto& e : package.entries()) {
                if (e.key.type == kImageType && e.key.instance == kLotPicture) {
                    if (auto decoded = ts2::image::decode(package.read(e))) {
                        picture = std::move(*decoded);
                    }
                    break;
                }
            }
        } catch (const std::exception& ex) {
            ts2::diag::warnOnce(std::string("lot catalog: a lot whose picture can't be read is shown without one: ") + ex.what());
        }
        out.push_back(std::move(picture));
    }
    return out;
}

} // namespace

NeighborhoodUi::NeighborhoodUi(ts2::ui::Screen& screen, ts2::content::UiData& data, int width, int height)
    : screen_(screen), data_(data), width_(width), height_(height) {}

NeighborhoodUi::Content NeighborhoodUi::read(const fs::path& folder, const fs::path& user) {
    return {homelessFamilies(folder), catalogLots(user), livedInLots(folder), lotCards(folder)};
}

void NeighborhoodUi::open(const fs::path& folder, const std::string& name, Content content) {
    close();
    // The panel's top left at the screen's, its puck in the bottom left corner (the game keeps the
    // puck at the bottom of the panel, which it sizes to the screen).
    panel_ = screen_.load(kScripts, kPanel, 0, 0);
    puck_ = screen_.find(panel_, kPuck);
    screen_.move(puck_, 0, height_ - screen_.height(puck_));
    chooseVersions(screen_, puck_, folder);
    for (const int c : screen_.children(panel_)) {
        if (screen_.id(c) == kTopBar && screen_.visible(c)) {
            chooseVersions(screen_, c, folder);
        }
    }
    const int cityName = screen_.findShown(panel_, kCityName);
    screen_.setText(cityName, name);
    // The top bar's background is as long as the name needs.
    if (const int bar = screen_.parent(cityName); bar >= 0) {
        const int background = screen_.find(bar, kTopBarBackground);
        screen_.resize(background, screen_.x(cityName) + kNameToBarEnd + screen_.textWidth(cityName) - screen_.x(background),
                       screen_.height(background));
    }

    // The bins, above the puck, hidden until the puck opens one.
    const int binX = screen_.x(puck_) + kBinX, binY = screen_.y(puck_) + kBinY;
    families_ = screen_.load(kScripts, kFamilyBin, binX, binY);
    chooseVersions(screen_, families_, folder, {kMoveFamily, kNewFamily});
    const auto college = childrenWithId(screen_, families_, kToCollege);
    for (std::size_t i = 0; i < college.size(); ++i) {
        screen_.setVisible(college[i], i == 0);
        screen_.setEnabled(college[i], false);  // no college attached
    }
    const int familyHolder = screen_.find(families_, kFamilyStripHolder);
    screen_.setVisible(screen_.find(familyHolder, kTallStrip), false);
    familyStrip_ = {screen_.find(familyHolder, kStripRow), {}};
    familyPictures_ = std::move(content.familyPictures);
    occupied_ = std::move(content.occupied);
    layoutStrip(familyStrip_, familyPictures_);
    screen_.setVisible(families_, false);

    lots_ = screen_.load(kScripts, kLotBin, binX, binY);
    // The lots view: its background (the first of its versions), its strip and filter. The others
    // are put away, and Lots & Houses and All are shown chosen.
    const auto backgrounds = childrenWithId(screen_, lots_, kLotsBackground);
    for (std::size_t i = 0; i < backgrounds.size(); ++i) {
        screen_.setVisible(backgrounds[i], i == 0);
    }
    for (const int c : childrenWithId(screen_, lots_, kEmptyLotsBackground)) {
        screen_.setVisible(c, false);
    }
    for (const std::uint32_t id : {kEmptyLotsView, kEmptyLotsSizes, kSpecialtyView}) {
        for (const int c : childrenWithId(screen_, lots_, id)) {
            screen_.setVisible(c, false);
        }
    }
    const int lotsView = screen_.find(lots_, kLotsView);
    screen_.setVisible(lotsView, true);
    chooseVersions(screen_, lots_, folder);
    for (const int c : childrenWithId(screen_, lots_, kLotsButton)) {
        screen_.setChecked(c, true);
    }
    screen_.setChecked(screen_.find(lotsView, kAllLots), true);
    const int lotHolder = screen_.find(lotsView, 0x0000c300);
    screen_.setVisible(screen_.find(lotHolder, kTallStrip), false);
    lotStrip_ = {screen_.find(lotHolder, kStripRow), {}};
    lotPictures_ = std::move(content.lotPictures);
    layoutStrip(lotStrip_, lotPictures_);
    screen_.setVisible(lots_, false);
    // The lot card, loaded with the neighborhood and hidden until a lot is clicked.
    card_ = screen_.load(kScripts, kLotCard, 0, 0);
    for (const int c : screen_.children(card_)) {
        screen_.setVisible(c, false);
    }
    screen_.setVisible(card_, false);
    cards_ = std::move(content.cards);
    cardLot_ = 0;
    tag_ = screen_.addWindows({tagWindow("IGZWinGen", {{"image", std::format("{{{:08x},{:08x}}}", kTagImageGroup, kTagBox)}, {"edgeimage", "yes"}})}, 0, 0);
    tagIcon_ = screen_.addWindows({tagWindow("IGZWinBMP", {{"image", std::format("{{{:08x},{:08x}}}", kTagImageGroup, kTagSun)}})}, 0, 0, tag_);
    tagText_ = screen_.addWindows({tagWindow("IGZWinText", {{"font", kTagFont}})}, 0, 0, tag_);
    screen_.setTextColor(tagText_, {0, 0, 0, 1});
    if (auto box = data_.image(kTagImageGroup, kTagBox); box && box->width >= kTagBoxSize && box->height >= kTagBoxSize) {
        ts2::tga::Pixels used;
        used.width = used.height = kTagBoxSize;
        used.rgba.resize(std::size_t{kTagBoxSize} * kTagBoxSize * 4);
        for (std::uint32_t y = 0; y < kTagBoxSize; ++y) {
            std::copy_n(&box->rgba[std::size_t{y} * box->width * 4], kTagBoxSize * 4, &used.rgba[std::size_t{y} * kTagBoxSize * 4]);
        }
        screen_.setImage(tag_, std::move(used));
    }
    screen_.setPassive(tag_);
    screen_.setVisible(tag_, false);
    tagLot_ = 0;
    options_ = screen_.load(kScripts, kOptionsMenu, 0, 0);
    if (options_ >= 0) {
        const int mark = anchorMark(screen_, options_);
        if (mark >= 0) {
            const std::string& caption = screen_.text(mark);
            const int anchor = screen_.find(puck_, static_cast<std::uint32_t>(std::strtoul(caption.c_str(), nullptr, 16)));
            if (anchor >= 0) {
                const auto to = onScreen(screen_, anchor), from = onScreen(screen_, mark);
                screen_.move(options_, screen_.x(options_) + to[0] - from[0], screen_.y(options_) + to[1] - from[1]);
            }
        }
        screen_.setVisible(options_, false);
    }
    // The puck and top bar are drawn over the bins and the options.
    screen_.raise(panel_);
    bin_ = Bin::None;
}

void NeighborhoodUi::close() {
    // Removed rather than hidden: the next neighborhood loads its own panel and bins.
    for (const int w : {panel_, families_, lots_, card_, tag_, options_}) {
        screen_.remove(w);
    }
    panel_ = families_ = lots_ = puck_ = card_ = tag_ = tagIcon_ = tagText_ = options_ = -1;
    tagLot_ = 0;
    cards_.clear();
    cardLot_ = 0;
    hidden_.clear();
    familyStrip_ = {};
    lotStrip_ = {};
    bin_ = Bin::None;
}

// The puck shows one of its bins or its options at a time, as the game does: opening one closes
// the other.
void NeighborhoodUi::showBin(Bin bin) {
    bin_ = bin;
    if (bin != Bin::None) {
        showOptions(false);
    }
    screen_.setVisible(families_, bin == Bin::Families);
    screen_.setVisible(lots_, bin == Bin::Lots);
    for (const int c : childrenWithId(screen_, puck_, kPuckFamilies)) {
        screen_.setChecked(c, bin == Bin::Families);
    }
    screen_.setChecked(screen_.find(puck_, kPuckLots), bin == Bin::Lots);
}

NeighborhoodUi::Action NeighborhoodUi::clicked(int window) {
    const std::uint32_t id = screen_.id(window);
    if (id == kMainMenu) {
        return Action::MainMenu;
    }
    if (screen_.parent(window) == puck_ && id == kPuckOptions) {
        showOptions(!screen_.visible(options_));
    } else if (options_ >= 0 && window == screen_.find(options_, kOptionsQuit)) {
        return Action::Quit;
    } else if (options_ >= 0 && window == screen_.find(options_, kOptionsMainMenu)) {
        return Action::MainMenu;
    } else if (screen_.parent(window) == puck_ && id == kPuckFamilies) {
        showBin(bin_ == Bin::Families ? Bin::None : Bin::Families);
    } else if (screen_.parent(window) == puck_ && id == kPuckLots) {
        showBin(bin_ == Bin::Lots ? Bin::None : Bin::Lots);
    } else if (id == kFamilyClose || id == kCommunityClose) {
        hideCard();
    } else if (id == kFamilyEnter && cardLot_ != 0) {
        return Action::EnterLot;
    }
    return Action::None;
}

void NeighborhoodUi::hideAll(bool hide) {
    if (hide && hidden_.empty()) {
        for (const int w : {panel_, families_, lots_, card_, tag_, options_}) {
            if (w >= 0) {
                hidden_.emplace_back(w, screen_.visible(w));
                screen_.setVisible(w, false);
            }
        }
    } else if (!hide) {
        for (const auto& [w, shown] : hidden_) {
            screen_.setVisible(w, shown);
        }
        hidden_.clear();
    }
}

void NeighborhoodUi::showOptions(bool show) {
    if (options_ < 0) {
        return;
    }
    if (show) {
        showBin(Bin::None);
    }
    screen_.setVisible(options_, show);
    screen_.setChecked(screen_.find(puck_, kPuckOptions), show);
}

bool NeighborhoodUi::clickable(std::uint32_t lot) const {
    const auto card = cards_.find(lot);
    return card != cards_.end() && !card->second.forSale && (!card->second.community || bin_ == Bin::Lots);
}

void NeighborhoodUi::showCard(std::uint32_t lot, int lotX) {
    const auto found = cards_.find(lot);
    if (card_ < 0 || found == cards_.end() || found->second.forSale) {
        return;
    }
    const LotCard& card = found->second;
    for (const int c : screen_.children(card_)) {
        screen_.setVisible(c, false);
    }
    if (card.community) {
        const int layout = screen_.find(card_, kCommunityLayout);
        screen_.setVisible(layout, true);
        screen_.setText(screen_.find(layout, kCommunityAddress), card.address);
        if (!card.text.empty()) {
            screen_.setText(screen_.find(layout, kCommunityText), card.text);
        }
    } else {
        const int layout = screen_.find(card_, kFamilyLayout);
        screen_.setVisible(layout, true);
        screen_.setText(screen_.find(layout, kFamilyName), card.name);
        screen_.setText(screen_.find(layout, kFamilyAddress), card.address);
        // The worth the game shows here is more than the family's funds, and how it adds up is not known
        // yet (open question UI-30).
        screen_.setText(screen_.find(layout, kFamilyFunds), {});
        screen_.setText(screen_.find(layout, kFamilyBio), card.text);
        if (card.picture.width > 0) {
            screen_.setImage(screen_.find(layout, kFamilyPicture), card.picture);
        }
    }
    // Beside the lot, on the side away from the screen's middle, and centred top to bottom. A card
    // that would run off the right edge keeps to the screen.
    const int w = screen_.width(card_), h = screen_.height(card_), half = width_ / 2;
    const int at = std::clamp(lotX, 0, width_);
    int x = 0;
    if (at < half) {
        x = at + kCardGap;
        if (x + w > width_) {
            x = w > half ? width_ - w : half;
        }
    } else {
        x = std::max(0, at - w - kCardGap);
    }
    screen_.move(card_, x, (height_ - h) / 2);
    screen_.setVisible(card_, true);
    screen_.raise(card_);
    cardLot_ = lot;
}

void NeighborhoodUi::showTag(std::uint32_t lot, int x, int y) {
    const auto found = cards_.find(lot);
    if (tag_ < 0 || found == cards_.end()) {
        if (tag_ >= 0) {
            screen_.setVisible(tag_, false);
        }
        tagLot_ = 0;
        return;
    }
    if (lot != tagLot_) {
        const LotCard& card = found->second;
        const std::string text = card.forSale ? data_.text(kNeighborhoodText, kForSale) : card.community ? card.address : card.name;
        const bool sun = !card.community;
        // The box fits the icon and the name with 16 pixels round them. Both are centred top to bottom.
        const auto size = screen_.textSize(kTagFont, text);
        const int left = kTagPadding + (sun ? kTagIcon : 0) + kTagTextGap;
        const int w = left + size[0] + kTagTextGap + kTagPadding, h = 2 * kTagPadding + std::max(sun ? kTagIcon : 0, size[1]);
        screen_.resize(tag_, w, h);
        screen_.setVisible(tagIcon_, sun);
        screen_.resize(tagIcon_, kTagIcon, kTagIcon);
        screen_.move(tagIcon_, kTagPadding, (h - kTagIcon) / 2);
        screen_.setText(tagText_, text);
        screen_.resize(tagText_, size[0] + 2, size[1]);
        screen_.move(tagText_, left, (h - size[1]) / 2);
        tagLot_ = lot;
    }
    // Centred across the point, its box's bottom 28 pixels above it.
    screen_.move(tag_, x - screen_.width(tag_) / 2, y - kTagAbove - screen_.height(tag_));
    screen_.setVisible(tag_, true);
}

void NeighborhoodUi::hideCard() {
    if (card_ >= 0) {
        screen_.setVisible(card_, false);
    }
    cardLot_ = 0;
}

void NeighborhoodUi::layoutStrip(Strip& strip, const std::vector<ts2::tga::Pixels>& pictures) {
    const int row = strip.container;
    if (row < 0) {
        return;
    }
    // As many tiles as the items need, at least 4, at most what fits before the screen's right edge.
    int rowLeft = 0;
    for (int w = row; w >= 0; w = screen_.parent(w)) {
        rowLeft += screen_.x(w);
    }
    const int fit = std::max(kMinTiles, (width_ - kScreenRightGap - rowLeft - kTileLeft - kRowTail) / kTileStep);
    const int count = static_cast<int>(pictures.size());
    const int n = std::clamp(count, kMinTiles, fit);
    const int rowWidth = kTileLeft + n * kTileStep + kRowTail;
    const bool more = count > n;
    screen_.resize(row, rowWidth, screen_.height(row));
    screen_.resize(screen_.find(row, kStripMiddle), rowWidth - 25 - 30, screen_.height(screen_.find(row, kStripMiddle)));
    screen_.move(screen_.find(row, kStripRightCap), rowWidth - 25, 0);
    screen_.setVisible(screen_.find(row, kStripBack), false);
    const int next = screen_.find(row, kStripNext);
    screen_.move(next, rowWidth - 50, screen_.y(next));
    screen_.setVisible(next, more);
    const int taller = screen_.find(row, kStripTaller);
    screen_.move(taller, rowWidth - 31, screen_.y(taller));
    screen_.setVisible(taller, more);
    const int corner = screen_.find(row, kStripCorner);
    screen_.move(corner, rowWidth - 44, screen_.y(corner));
    for (const int c : screen_.children(corner)) {
        screen_.setEnabled(c, false);  // nothing chosen in the strip
    }
    screen_.setVisible(screen_.find(row, kStripScroll), more);
    screen_.setVisible(screen_.find(row, kStripArea), false);
    // A tile per item on the first page, its picture inside it.
    for (int i = 0; i < std::min(count, n); ++i) {
        const int tile = screen_.load(kScripts, kTile, kTileLeft + i * kTileStep, kTileTop, row);
        if (tile < 0) {
            break;
        }
        if (pictures[static_cast<std::size_t>(i)].width > 0) {
            screen_.setImage(screen_.find(tile, kTilePicture), pictures[static_cast<std::size_t>(i)]);
        }
        strip.tiles.push_back(tile);
    }
}

} // namespace app
