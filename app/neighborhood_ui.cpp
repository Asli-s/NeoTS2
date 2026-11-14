#include "neighborhood_ui.hpp"

#include "neighborhood.hpp"

#include "content/install.hpp"
#include "content/neighborhood.hpp"
#include "content/nhood_catalog.hpp"
#include "content/story_album.hpp"
#include "content/ui_meters.hpp"
#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/fami.hpp"
#include "formats/image.hpp"
#include "formats/lot_description.hpp"
#include "formats/neighborhood_id.hpp"
#include "formats/str.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <utility>

namespace app {

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kScripts = 0xA99D8A11;
// The panel (the puck, the top bar) and its windows by id.
constexpr std::uint32_t kPanel = 0x49000000, kMainMenu = 0x00000002, kCityName = 0x00000003;
constexpr std::uint32_t kPuck = 0x4be6ed7d, kTopBar = 0x4be6ed7e, kTopBarBackground = 0x00000001;
constexpr std::uint32_t kPuckFamilies = 0x2be6f563, kPuckLots = 0x4be6f571, kPuckOptions = 0x2bf08bec;
// The Decorations catalog (the puck's Decorations button, F3): its panel and strip holder, its
// section buttons, and its tools.
constexpr std::uint32_t kPuckDecorations = 0x2d51756d, kDecorationBin = 0x4D517180, kDecorationStripHolder = 0x2d51756d;
constexpr std::pair<std::uint32_t, const char*> kDecorationSorts[] = {
    {0x0000a100, "flora"}, {0x0000a200, "stone"}, {0x0000a300, "landmark"}, {0x0000a400, "effects"}, {0x0000a500, "misc"}};
constexpr std::uint32_t kDecorationHand = 0x000000a5, kDecorationEyedropper = 0x000000a4, kDecorationUndo = 0x000000a1,
                        kDecorationRedo = 0x000000a2;
constexpr std::uint32_t kTileButton = 0x00000003;  // a strip tile's button
// The puck's camera buttons in CameraButton's order, from the left Rotate.
constexpr std::uint32_t kCameraButtons[] = {0x2be6fb52, 0x2be6fb57, 0x2be6fb48, 0x2be6fb4e};
constexpr std::uint32_t kDayNight = 0x0fb31a16;
// The puck's Camera Snapshot (C) and Video Capture (V) buttons.
constexpr std::uint32_t kSnapshot = 0xcd094d26, kVideoCapture = 0xed094d4f;
// The neighborhood's own options menu and its Main Menu and Quit buttons. A hidden 1x1 window in it
// is its anchor, whose caption names the puck window it goes on ("aabbccdd:tl").
constexpr std::uint32_t kOptionsMenu = 0x49001028, kOptionsMainMenu = 0x0000a221, kOptionsQuit = 0x0000a220;
constexpr std::uint32_t kOptionsButtons = 0x0000a200;  // the frame holding the menu's buttons
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
constexpr std::uint32_t kSpecialtyBackground = 0x0000f200, kSpecialtyButton = 0x0000f100, kLotStripHolder = 0x0000c300,
                        kOccupiedLots = 0x0000c400, kUnoccupiedLots = 0x0000c500;
// The Empty Lots view's ordinary and beach lot sizes (a button per size in each) and their buttons.
constexpr std::uint32_t kEmptyLotSizes = 0x0000d400, kBeachLotSizes = 0x0000d500, kOrdinaryLotsButton = 0x0000d600,
                        kBeachLotsButton = 0x0000d700;
// The lot bin's tools: Move or Rotate Lot, Move to Lots & Houses Bin, Bulldoze.
constexpr std::uint32_t kMoveLot = 0x0000a000, kLotToBin = 0x0000e000, kBulldoze = 0x0000b000;
// A strip of tiles: its row, the row's parts, and the tile each item shows in.
constexpr std::uint32_t kStripRow = 0x00001000, kStripLeftCap = 0x00001001, kStripMiddle = 0x00001002, kStripRightCap = 0x00001003,
                        kStripBack = 0x00001004, kStripNext = 0x00001005, kStripArea = 0x00001006, kStripScroll = 0x00001008,
                        kStripTaller = 0x00001009, kStripCorner = 0x0000100a;
constexpr std::uint32_t kTile = 0x49001006, kTilePicture = 0x00000002;
// Tiles start 52 into the row, 95 apart. The row ends 51 past the last. A row holds at least 4 and
// as many as fit before 63 from the screen's right edge.
constexpr int kTileLeft = 52, kTileStep = 95, kTileTop = 9, kRowTail = 51, kMinTiles = 4, kScreenRightGap = 63;
// The expanded strip's two rows of tiles: from 11 down (its tile area's top), a tile's height apart.
constexpr int kTallTileTop = 11, kTileRowStep = 93;
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
// The neighborhood's story (Neighborhood Story), opened by the puck's story button: its title
// ("Welcome to %s!"), picture (cut to the shape of its own image), caption, page counter ("%ld of
// %ld"), Previous and Next, the text shown when there are no pages, Done, and Edit Neighborhood
// Story (the story editor isn't built yet).
constexpr std::uint32_t kStoryViewer = 0x49001030, kPuckStory = 0xacd99323;
constexpr std::uint32_t kStoryTitle = 0x2d0a8e57, kStoryPicture = 0x4d0a8d83, kStoryCaption = 0xcd0a8ef7, kStoryCounter = 0x2d0e41dc,
                        kStoryPrevious = 0x2d0a8f5a, kStoryNext = 0x2d0a8f55, kStoryNone = 0x4d7407b2, kStoryDone = 0xcc800571,
                        kStoryEdit = 0x0cb23b3c;
constexpr std::uint32_t kStoryShape = 0xa96d0603;  // the picture window's image, in the UI image group
// Snapshots are read at most this wide or tall (the game saves them 640 x 480 or so).
constexpr std::uint32_t kMaxSnapshotSide = 2048;
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

// The neighborhood's id (its story album's too), from its package. Empty when it can't be read.
std::optional<std::int32_t> readNeighborhoodId(const fs::path& folder) {
    try {
        ts2::dbpf::Package package{ts2::content::neighborhoodPackage(folder)};
        for (const auto& e : package.entries()) {
            if (e.key.type == ts2::nhoodid::kType) {
                return ts2::nhoodid::parse(package.read(e)).id;
            }
        }
    } catch (const std::exception& ex) {
        ts2::diag::warnOnce(std::string("neighborhood story: the neighborhood's id can't be read, so it has no story or snapshots: ") + ex.what());
    }
    return std::nullopt;
}

// The neighborhood's story: the album named by the neighborhood's id in the user folder's
// Storytelling folder, each entry's snapshot read from there. A snapshot that can't be read leaves
// its page without a picture.
std::vector<NeighborhoodUi::StoryPage> storyPages(std::optional<std::int32_t> id, const fs::path& user) {
    std::vector<NeighborhoodUi::StoryPage> out;
    if (!id) {
        return out;
    }
    const fs::path storytelling = user / "Storytelling";
    const fs::path file = ts2::content::storyAlbumFile(storytelling, static_cast<std::uint32_t>(*id));
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return out;  // no story yet
    }
    const std::string xml{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    for (const auto& entry : ts2::content::parseStoryAlbum(xml).entries) {
        NeighborhoodUi::StoryPage page;
        page.text = entry.text;
        page.textResource = entry.textResource;
        std::ifstream picture(storytelling / entry.snapshot, std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(picture), std::istreambuf_iterator<char>()};
        if (auto decoded = bytes.empty() ? std::nullopt : ts2::image::decode(bytes, kMaxSnapshotSide)) {
            page.picture = std::move(*decoded);
        } else {
            ts2::diag::warnOnce("neighborhood story: a snapshot can't be read, so its page shows no picture: " + entry.snapshot);
        }
        out.push_back(std::move(page));
    }
    return out;
}

// The Decorations catalog: the entries it offers, in guid order, each with its picture (none when
// the catalog has no picture of it).
std::vector<NeighborhoodUi::DecorationItem> decorationItems(const fs::path& install) {
    std::vector<NeighborhoodUi::DecorationItem> out;
    try {
        const auto products = ts2::content::findProducts(install);
        auto pictures = ts2::content::neighborhoodObjectThumbnails(products);
        for (const auto& [guid, type] : ts2::content::neighborhoodObjectTypes(products)) {
            if (!type.shownInCatalog || type.model.empty() || type.sort.empty()) {
                continue;
            }
            NeighborhoodUi::DecorationItem item{guid, type.sort, {}};
            if (const auto p = pictures.find(guid); p != pictures.end()) {
                item.picture = std::move(p->second);
            }
            out.push_back(std::move(item));
        }
    } catch (const std::exception& ex) {
        ts2::diag::warnOnce(std::string("decorations: the catalog can't be read, so it is empty: ") + ex.what());
    }
    return out;
}

} // namespace

NeighborhoodUi::NeighborhoodUi(ts2::ui::Screen& screen, ts2::content::UiData& data, int width, int height)
    : screen_(screen), data_(data), width_(width), height_(height) {}

NeighborhoodUi::Content NeighborhoodUi::read(const fs::path& folder, const fs::path& user, const fs::path& install) {
    const auto id = readNeighborhoodId(folder);
    return {homelessFamilies(folder), catalogLots(user), livedInLots(folder), lotCards(folder), storyPages(id, user), id, decorationItems(install)};
}

void NeighborhoodUi::open(const fs::path& folder, const std::string& name, Content content) {
    close();
    // The panel covers the screen, and each time its size is set the game puts the puck in its
    // bottom left corner.
    panel_ = screen_.load(kScripts, kPanel, 0, 0);
    screen_.resize(panel_, width_, height_);
    puck_ = screen_.find(panel_, kPuck);
    screen_.move(puck_, 0, height_ - screen_.height(puck_));
    chooseVersions(screen_, puck_, folder);
    screen_.setEnabled(screen_.find(puck_, kVideoCapture), false);  // video capture isn't built yet
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
    // Making a family (Create-a-Sim) and moving one in aren't built yet.
    for (const std::uint32_t id : {kMoveFamily, kNewFamily}) {
        for (const int c : childrenWithId(screen_, families_, id)) {
            screen_.setEnabled(c, false);
        }
    }
    const int familyHolder = screen_.find(families_, kFamilyStripHolder);
    familyStrip_ = {screen_.find(familyHolder, kStripRow), screen_.find(familyHolder, kTallStrip)};
    familyPictures_ = std::move(content.familyPictures);
    occupied_ = std::move(content.occupied);
    layoutStrip(familyStrip_, familyPictures_);
    screen_.setVisible(families_, false);

    lots_ = screen_.load(kScripts, kLotBin, binX, binY);
    chooseVersions(screen_, lots_, folder);
    // Of each view's backgrounds (a version for each product), the first.
    for (const std::uint32_t id : {kLotsBackground, kEmptyLotsBackground, kSpecialtyBackground}) {
        lotBackgrounds_[id] = childrenWithId(screen_, lots_, id).empty() ? -1 : childrenWithId(screen_, lots_, id).front();
    }
    const int lotsView = screen_.find(lots_, kLotsView);
    screen_.setChecked(screen_.find(lotsView, kAllLots), true);
    const int lotHolder = screen_.find(lotsView, kLotStripHolder);
    lotStrip_ = {screen_.find(lotHolder, kStripRow), screen_.find(lotHolder, kTallStrip)};
    lotPictures_ = std::move(content.lotPictures);
    layoutStrip(lotStrip_, lotPictures_);
    // The specialty lots aren't read yet: their strip is empty.
    const int specialtyHolder = screen_.find(screen_.find(lots_, kSpecialtyView), kLotStripHolder);
    specialtyStrip_ = {screen_.find(specialtyHolder, kStripRow), screen_.find(specialtyHolder, kTallStrip)};
    layoutStrip(specialtyStrip_, {});
    // What changes the neighborhood isn't built yet: placing an empty lot, moving, packing away and
    // bulldozing lots. Nor is the Occupied and Unoccupied filter: All stays chosen.
    for (const std::uint32_t id : {kMoveLot, kLotToBin, kBulldoze}) {
        screen_.setEnabled(screen_.find(lots_, id), false);
    }
    screen_.setEnabled(screen_.find(lotsView, kOccupiedLots), false);
    screen_.setEnabled(screen_.find(lotsView, kUnoccupiedLots), false);
    const int emptyView = screen_.find(lots_, kEmptyLotsView);
    for (const std::uint32_t sizes : {kEmptyLotSizes, kBeachLotSizes}) {
        for (const int c : screen_.children(screen_.find(emptyView, sizes))) {
            screen_.setEnabled(c, false);
        }
    }
    showLotView(LotView::Lots);
    screen_.setVisible(lots_, false);
    // The Decorations catalog, opening on its plants. Undo and Redo aren't built yet (and the
    // strip's Delete Custom Content stays greyed, as nothing in it is chosen).
    decorations_ = screen_.load(kScripts, kDecorationBin, binX, binY);
    if (decorations_ >= 0) {
        chooseVersions(screen_, decorations_, folder);
        const int holder = screen_.find(decorations_, kDecorationStripHolder);
        decorationStrip_ = {screen_.find(holder, kStripRow), screen_.find(holder, kTallStrip)};
        for (const std::uint32_t id : {kDecorationUndo, kDecorationRedo}) {
            screen_.setEnabled(screen_.find(decorations_, id), false);
        }
        decorationItems_ = std::move(content.decorations);
        showDecorationSort("flora");
        screen_.setVisible(decorations_, false);
    }
    decorationTool_ = DecorationTool::None;
    chosenDecoration_ = 0;
    // The lot card, loaded with the neighborhood and hidden until a lot is clicked.
    card_ = screen_.load(kScripts, kLotCard, 0, 0);
    for (const int c : screen_.children(card_)) {
        screen_.setVisible(c, false);
    }
    screen_.setVisible(card_, false);
    cards_ = std::move(content.cards);
    cardLot_ = 0;
    // The story, hidden until the puck's story button opens it. Its title names the neighborhood.
    story_ = screen_.load(kScripts, kStoryViewer, 0, 0);
    if (story_ >= 0) {
        const int title = screen_.find(story_, kStoryTitle);
        std::string welcome = screen_.text(title);
        if (const std::size_t at = welcome.find("%s"); at != std::string::npos) {
            welcome.replace(at, 2, name);
        }
        screen_.setText(title, welcome);
        storyCounter_ = screen_.text(screen_.find(story_, kStoryCounter));
        screen_.setEnabled(screen_.find(story_, kStoryCaption), false);  // shown, not edited: editing isn't built yet
        screen_.setEnabled(screen_.find(story_, kStoryEdit), false);
        // The picture window's own image is the shape a snapshot is cut to.
        if (auto shape = data_.image(kTagImageGroup, kStoryShape)) {
            storyShape_ = std::move(*shape);
        }
        screen_.setVisible(story_, false);
    }
    storyPages_ = std::move(content.story);
    neighborhoodId_ = content.neighborhoodId;
    for (auto& page : storyPages_) {
        if (page.text.empty() && page.textResource != 0) {
            page.text = data_.text(ts2::content::kStoryTextGroup, page.textResource);
        }
    }
    storyPage_ = 0;
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
        // The script's area stops short of the buttons. The game shows the menu out to the right end
        // of the frame holding them, so they all take the mouse.
        if (const int buttons = screen_.find(options_, kOptionsButtons); buttons >= 0) {
            screen_.resize(options_, screen_.x(buttons) + screen_.width(buttons), screen_.height(options_));
        }
        optionsMenu_.setMenu(options_);
        gameOptions_.load(GameOptionsPanel::Place::Neighborhood);
        graphicsOptions_.load(GraphicsOptionsPanel::Place::Neighborhood);
    }
    // The puck and top bar are drawn over the bins and the options.
    screen_.raise(panel_);
    bin_ = Bin::None;
}

void NeighborhoodUi::setSize(int width, int height) {
    const int down = height - height_;
    width_ = width;
    height_ = height;
    if (panel_ < 0) {
        return;
    }
    screen_.resize(panel_, width_, height_);
    for (const int window : {puck_, families_, lots_, options_, decorations_}) {
        if (window >= 0) {
            screen_.move(window, screen_.x(window), screen_.y(window) + down);
        }
    }
}

void NeighborhoodUi::close() {
    // Removed rather than hidden: the next neighborhood loads its own panel and bins.
    gameOptions_.remove();
    graphicsOptions_.remove();
    optionsMenu_.setMenu(-1);
    for (const int w : {panel_, families_, lots_, card_, tag_, options_, story_, decorations_}) {
        screen_.remove(w);
    }
    panel_ = families_ = lots_ = puck_ = card_ = tag_ = tagIcon_ = tagText_ = options_ = story_ = decorations_ = -1;
    decorationStrip_ = {};
    decorationItems_.clear();
    decorationShown_.clear();
    decorationPictures_.clear();
    decorationTool_ = DecorationTool::None;
    chosenDecoration_ = 0;
    storyPages_.clear();
    neighborhoodId_.reset();
    storyPage_ = 0;
    tagLot_ = 0;
    cards_.clear();
    cardLot_ = 0;
    hidden_.clear();
    familyStrip_ = {};
    lotStrip_ = {};
    specialtyStrip_ = {};
    lotBackgrounds_.clear();
    bin_ = Bin::None;
}

// The puck shows one of its bins or its options at a time, as the game does: opening one closes
// the other.
void NeighborhoodUi::showBin(Bin bin) {
    if (bin != Bin::Decorations && bin_ == Bin::Decorations) {
        setDecorationTool(DecorationTool::None);
    }
    bin_ = bin;
    if (bin != Bin::None) {
        showOptions(false);
        showStory(false);
    }
    screen_.setVisible(families_, bin == Bin::Families);
    screen_.setVisible(lots_, bin == Bin::Lots);
    if (decorations_ >= 0) {
        screen_.setVisible(decorations_, bin == Bin::Decorations);
    }
    screen_.setChecked(screen_.find(puck_, kPuckDecorations), bin == Bin::Decorations);
    for (const int c : childrenWithId(screen_, puck_, kPuckFamilies)) {
        screen_.setChecked(c, bin == Bin::Families);
    }
    screen_.setChecked(screen_.find(puck_, kPuckLots), bin == Bin::Lots);
}

NeighborhoodUi::CameraButton NeighborhoodUi::heldCameraButton() const {
    if (puck_ < 0) {
        return CameraButton::None;
    }
    for (std::size_t i = 0; i < std::size(kCameraButtons); ++i) {
        if (const int button = screen_.find(puck_, kCameraButtons[i]); button >= 0 && screen_.held(button)) {
            return static_cast<CameraButton>(i + 1);
        }
    }
    return CameraButton::None;
}

NeighborhoodUi::Action NeighborhoodUi::clicked(int window) {
    if (graphicsOptions_.clicked(window)) {
        return graphicsOptions_.takeChanged() ? Action::GraphicsOptionChanged : Action::None;
    }
    if (gameOptions_.clicked(window)) {
        return gameOptions_.takeChanged() ? Action::GameOptionChanged : Action::None;
    }
    const std::uint32_t id = screen_.id(window);
    if (id == kMainMenu) {
        return Action::MainMenu;
    }
    if (screen_.parent(window) == puck_ && id == kPuckOptions) {
        showOptions(!screen_.visible(options_));
    } else if (screen_.parent(window) == puck_ && id == kSnapshot) {
        return Action::Snapshot;
    } else if (screen_.parent(window) == puck_ && id == kDayNight) {
        return Action::DayNight;
    } else if (options_ >= 0 && window == screen_.find(options_, kOptionsQuit)) {
        return Action::Quit;
    } else if (options_ >= 0 && window == screen_.find(options_, kOptionsMainMenu)) {
        return Action::MainMenu;
    } else if (screen_.parent(window) == puck_ && id == kPuckFamilies) {
        showBin(bin_ == Bin::Families ? Bin::None : Bin::Families);
    } else if (screen_.parent(window) == puck_ && id == kPuckLots) {
        showBin(bin_ == Bin::Lots ? Bin::None : Bin::Lots);
    } else if (screen_.parent(window) == puck_ && id == kPuckDecorations) {
        showBin(bin_ == Bin::Decorations ? Bin::None : Bin::Decorations);
    } else if (stripClicked(familyStrip_, familyPictures_, window) || stripClicked(lotStrip_, lotPictures_, window) ||
               stripClicked(specialtyStrip_, {}, window) || stripClicked(decorationStrip_, decorationPictures_, window)) {
        // A strip's page turned, or the strip expanded or collapsed.
    } else if (const auto item = stripItemAt(decorationStrip_, window); item && *item < decorationShown_.size()) {
        setDecorationTool(DecorationTool::Place, decorationShown_[*item]);
        return Action::DecorationTool;
    } else if (decorations_ >= 0 && window == screen_.find(decorations_, kDecorationHand)) {
        setDecorationTool(decorationTool_ == DecorationTool::Hand ? DecorationTool::None : DecorationTool::Hand);
        return Action::DecorationTool;
    } else if (decorations_ >= 0 && window == screen_.find(decorations_, kDecorationEyedropper)) {
        setDecorationTool(decorationTool_ == DecorationTool::Eyedropper ? DecorationTool::None : DecorationTool::Eyedropper);
        return Action::DecorationTool;
    } else if (const auto sort = std::find_if(std::begin(kDecorationSorts), std::end(kDecorationSorts), [&](const auto& s) { return s.first == id; });
               decorations_ >= 0 && sort != std::end(kDecorationSorts) && screen_.parent(screen_.parent(window)) == decorations_) {
        showDecorationSort(sort->second);
    } else if (lots_ >= 0 && screen_.parent(window) == lots_ && id == kLotsButton) {
        showLotView(LotView::Lots);
    } else if (lots_ >= 0 && screen_.parent(window) == lots_ && id == kEmptyLotsButton) {
        showLotView(LotView::EmptyLots);
    } else if (lots_ >= 0 && screen_.parent(window) == lots_ && id == kSpecialtyButton) {
        showLotView(LotView::Specialty);
    } else if (lots_ >= 0 && screen_.parent(window) == screen_.find(lots_, kEmptyLotsView) && (id == kOrdinaryLotsButton || id == kBeachLotsButton)) {
        showEmptyLotSizes(id == kBeachLotsButton);
    } else if (screen_.parent(window) == puck_ && id == kPuckStory) {
        showStory(!storyShown());
    } else if (storyShown() && window == screen_.find(story_, kStoryDone)) {
        showStory(false);
    } else if (storyShown() && window == screen_.find(story_, kStoryPrevious) && storyPage_ > 0) {
        showStoryPage(storyPage_ - 1);
    } else if (storyShown() && window == screen_.find(story_, kStoryNext) && storyPage_ + 1 < storyPages_.size()) {
        showStoryPage(storyPage_ + 1);
    } else if (id == kFamilyClose || id == kCommunityClose) {
        hideCard();
    } else if (id == kFamilyEnter && cardLot_ != 0) {
        return Action::EnterLot;
    }
    return Action::None;
}

void NeighborhoodUi::hideAll(bool hide) {
    if (hide && hidden_.empty()) {
        for (const int w : {panel_, families_, lots_, card_, tag_, options_, story_, decorations_}) {
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
        showStory(false);
    } else {
        optionsMenu_.close();
    }
    screen_.setVisible(options_, show);
    screen_.setChecked(screen_.find(puck_, kPuckOptions), show);
}

void NeighborhoodUi::functionKey(int n) {
    if (panel_ < 0 || !screen_.visible(panel_)) {
        return;
    }
    switch (n) {
    case 1: showBin(bin_ == Bin::Families ? Bin::None : Bin::Families); break;
    case 2: showBin(bin_ == Bin::Lots ? Bin::None : Bin::Lots); break;
    case 3: showBin(bin_ == Bin::Decorations ? Bin::None : Bin::Decorations); break;
    case 4: showStory(!storyShown()); break;
    case 5: showOptions(options_ >= 0 && !screen_.visible(options_)); break;
    default: break;
    }
}

void NeighborhoodUi::showStory(bool show) {
    if (story_ < 0) {
        return;
    }
    if (show) {
        showBin(Bin::None);
        showOptions(false);
        hideCard();
        // In the middle of the screen, on top, from its first page.
        screen_.move(story_, (width_ - screen_.width(story_)) / 2, (height_ - screen_.height(story_)) / 2);
        screen_.raise(story_);
        showStoryPage(0);
    }
    screen_.setVisible(story_, show);
    screen_.setChecked(screen_.find(puck_, kPuckStory), show);
}

bool NeighborhoodUi::storyShown() const { return story_ >= 0 && screen_.visible(story_); }

void NeighborhoodUi::showStoryPage(std::size_t page) {
    const bool any = !storyPages_.empty();
    storyPage_ = any ? std::min(page, storyPages_.size() - 1) : 0;
    screen_.setVisible(screen_.find(story_, kStoryNone), !any);
    const int picture = screen_.find(story_, kStoryPicture);
    const int caption = screen_.find(story_, kStoryCaption);
    const int counter = screen_.find(story_, kStoryCounter);
    screen_.setVisible(picture, any);
    screen_.setVisible(caption, any);
    screen_.setVisible(counter, any);
    screen_.setEnabled(screen_.find(story_, kStoryPrevious), any && storyPage_ > 0);
    screen_.setEnabled(screen_.find(story_, kStoryNext), any && storyPage_ + 1 < storyPages_.size());
    if (!any) {
        return;
    }
    const StoryPage& shown = storyPages_[storyPage_];
    // The snapshot fills the picture window, cut to its rounded shape.
    const auto w = static_cast<std::uint32_t>(screen_.width(picture)), h = static_cast<std::uint32_t>(screen_.height(picture));
    if (shown.picture.width > 0 && w > 0 && h > 0) {
        auto fitted = ts2::content::scaled(shown.picture, w, h);
        if (storyShape_.width > 0) {
            fitted = ts2::content::masked(fitted, ts2::content::scaled(storyShape_, w, h));
        }
        screen_.setImage(picture, std::move(fitted));
    } else {
        screen_.setVisible(picture, false);
    }
    screen_.setText(caption, shown.text);
    // The counter's own text ("%ld of %ld") with the page and the count put in.
    std::string count = storyCounter_;
    for (const std::size_t n : {storyPage_ + 1, storyPages_.size()}) {
        if (const std::size_t at = count.find("%ld"); at != std::string::npos) {
            count.replace(at, 3, std::to_string(n));
        }
    }
    screen_.setText(counter, count);
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
    if (strip.container < 0) {
        return;
    }
    for (const int tile : strip.tiles) {
        screen_.remove(tile);
    }
    strip.tiles.clear();
    // The expanded strip's windows have the row's ids plus 0x1000, and it has two rows of tiles.
    const bool tall = strip.expanded && strip.tall >= 0;
    const int row = tall ? strip.tall : strip.container;
    const std::uint32_t part = tall ? kTallStrip - kStripRow : 0;
    const int rows = tall ? 2 : 1;
    screen_.setVisible(strip.container, !tall);
    if (strip.tall >= 0) {
        screen_.setVisible(strip.tall, tall);
    }
    // As many tiles across as the items need, at least 4, at most what fits before the screen's
    // right edge.
    int rowLeft = 0;
    for (int w = row; w >= 0; w = screen_.parent(w)) {
        rowLeft += screen_.x(w);
    }
    const int fit = std::max(kMinTiles, (width_ - kScreenRightGap - rowLeft - kTileLeft - kRowTail) / kTileStep);
    const int count = static_cast<int>(pictures.size());
    const int n = std::clamp((count + rows - 1) / rows, kMinTiles, fit);
    strip.perPage = static_cast<std::size_t>(n * rows);
    // A page starts at a multiple of the page's size, and the last page is never past the items.
    if (strip.first >= pictures.size()) {
        strip.first = pictures.empty() ? 0 : (pictures.size() - 1) / strip.perPage * strip.perPage;
    }
    strip.first = strip.first / strip.perPage * strip.perPage;
    const bool before = strip.first > 0, after = strip.first + strip.perPage < pictures.size();
    const int rowWidth = kTileLeft + n * kTileStep + kRowTail;
    screen_.resize(row, rowWidth, screen_.height(row));
    // The windows holding the strip grow to hold it too: a click only reaches a button inside each
    // of its parents.
    for (int w = row, p = screen_.parent(row); p >= 0; w = p, p = screen_.parent(p)) {
        if (screen_.x(w) + screen_.width(w) > screen_.width(p)) {
            screen_.resize(p, screen_.x(w) + screen_.width(w), screen_.height(p));
        }
    }
    const int middle = screen_.find(row, kStripMiddle + part);
    screen_.resize(middle, rowWidth - 25 - 30, screen_.height(middle));
    screen_.move(screen_.find(row, kStripRightCap + part), rowWidth - 25, 0);
    screen_.setVisible(screen_.find(row, kStripBack + part), before);
    const int next = screen_.find(row, kStripNext + part);
    screen_.move(next, rowWidth - 50, screen_.y(next));
    screen_.setVisible(next, after);
    // Expand shows while a row can't hold every item; Collapse always shows on the expanded strip.
    const int taller = screen_.find(row, kStripTaller + part);
    screen_.move(taller, rowWidth - 31, screen_.y(taller));
    screen_.setVisible(taller, tall || count > n);
    const int corner = screen_.find(row, kStripCorner + part);
    screen_.move(corner, rowWidth - 44, screen_.y(corner));
    for (const int c : screen_.children(corner)) {
        screen_.setEnabled(c, false);  // nothing chosen in the strip
    }
    screen_.setVisible(screen_.find(row, kStripScroll + part), false);  // the pages are turned with Previous and Next
    screen_.setVisible(screen_.find(row, kStripArea + part), false);
    // A tile per item on the page, its picture inside it, row by row.
    const int top = tall ? kTallTileTop : kTileTop;
    for (std::size_t i = strip.first; i < pictures.size() && i < strip.first + strip.perPage; ++i) {
        const int at = static_cast<int>(i - strip.first);
        const int tile = screen_.load(kScripts, kTile, kTileLeft + at % n * kTileStep, top + at / n * kTileRowStep, row);
        if (tile < 0) {
            break;
        }
        if (pictures[i].width > 0) {
            screen_.setImage(screen_.find(tile, kTilePicture), pictures[i]);
        }
        strip.tiles.push_back(tile);
    }
}

void NeighborhoodUi::showLotView(LotView view) {
    if (lots_ < 0) {
        return;
    }
    // The view's background (only its first version) and its part, the others put away.
    const std::pair<std::uint32_t, std::uint32_t> views[] = {
        {kLotsBackground, kLotsView}, {kEmptyLotsBackground, kEmptyLotsView}, {kSpecialtyBackground, kSpecialtyView}};
    for (std::size_t i = 0; i < std::size(views); ++i) {
        const bool shown = static_cast<std::size_t>(view) == i;
        for (const int c : childrenWithId(screen_, lots_, views[i].first)) {
            screen_.setVisible(c, shown && c == lotBackgrounds_[views[i].first]);
        }
        for (const int c : childrenWithId(screen_, lots_, views[i].second)) {
            screen_.setVisible(c, shown);
        }
    }
    for (const int c : childrenWithId(screen_, lots_, kEmptyLotsSizes)) {
        screen_.setVisible(c, false);
    }
    // The tabs: the view's shown chosen.
    const std::pair<std::uint32_t, LotView> tabs[] = {
        {kLotsButton, LotView::Lots}, {kEmptyLotsButton, LotView::EmptyLots}, {kSpecialtyButton, LotView::Specialty}};
    for (const auto& [id, tabView] : tabs) {
        for (const int c : childrenWithId(screen_, lots_, id)) {
            screen_.setChecked(c, tabView == view);
        }
    }
    if (view == LotView::EmptyLots) {
        showEmptyLotSizes(false);
    }
}

void NeighborhoodUi::showEmptyLotSizes(bool beach) {
    const int view = screen_.find(lots_, kEmptyLotsView);
    screen_.setVisible(screen_.find(view, kEmptyLotSizes), !beach);
    screen_.setVisible(screen_.find(view, kBeachLotSizes), beach);
    screen_.setVisible(screen_.find(view, kStripRow), false);
    screen_.setChecked(screen_.find(view, kOrdinaryLotsButton), !beach);
    screen_.setChecked(screen_.find(view, kBeachLotsButton), beach);
}

void NeighborhoodUi::showDecorationSort(const std::string& sort) {
    decorationSort_ = sort;
    decorationShown_.clear();
    decorationPictures_.clear();
    for (const auto& item : decorationItems_) {
        if (item.sort == sort) {
            decorationShown_.push_back(item.guid);
            decorationPictures_.push_back(item.picture);
        }
    }
    for (const auto& [id, name] : kDecorationSorts) {
        screen_.setChecked(screen_.find(decorations_, id), name == sort);
    }
    decorationStrip_.first = 0;
    layoutStrip(decorationStrip_, decorationPictures_);
}

std::optional<std::size_t> NeighborhoodUi::stripItemAt(const Strip& strip, int window) const {
    for (int w = window; w >= 0; w = screen_.parent(w)) {
        const auto tile = std::find(strip.tiles.begin(), strip.tiles.end(), w);
        if (tile != strip.tiles.end()) {
            return strip.first + static_cast<std::size_t>(tile - strip.tiles.begin());
        }
    }
    return std::nullopt;
}

void NeighborhoodUi::setDecorationTool(DecorationTool tool, std::uint32_t guid) {
    decorationTool_ = tool;
    chosenDecoration_ = tool == DecorationTool::Place ? guid : 0;
    if (decorations_ < 0) {
        return;
    }
    screen_.setChecked(screen_.find(decorations_, kDecorationHand), tool == DecorationTool::Hand);
    screen_.setChecked(screen_.find(decorations_, kDecorationEyedropper), tool == DecorationTool::Eyedropper);
    // The chosen entry's tile shows pressed while it is held.
    for (std::size_t i = 0; i < decorationStrip_.tiles.size(); ++i) {
        const std::size_t item = decorationStrip_.first + i;
        screen_.setChecked(screen_.find(decorationStrip_.tiles[i], kTileButton),
                           tool == DecorationTool::Place && item < decorationShown_.size() && decorationShown_[item] == guid);
    }
}

bool NeighborhoodUi::stripClicked(Strip& strip, const std::vector<ts2::tga::Pixels>& pictures, int window) {
    const int parent = screen_.parent(window);
    if (strip.container < 0 || (parent != strip.container && parent != strip.tall)) {
        return false;
    }
    const std::uint32_t id = screen_.id(window) - (parent == strip.tall ? kTallStrip - kStripRow : 0);
    if (id == kStripBack && strip.first > 0) {
        strip.first -= std::min(strip.first, strip.perPage);
    } else if (id == kStripNext && strip.first + strip.perPage < pictures.size()) {
        strip.first += strip.perPage;
    } else if (id == kStripTaller) {
        strip.expanded = !strip.expanded;
    } else {
        return false;
    }
    layoutStrip(strip, pictures);
    return true;
}

} // namespace app
