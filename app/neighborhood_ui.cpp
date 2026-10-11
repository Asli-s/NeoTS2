#include "neighborhood_ui.hpp"

#include "neighborhood.hpp"

#include "content/neighborhood.hpp"
#include "formats/dbpf.hpp"
#include "formats/fami.hpp"
#include "formats/image.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace app {

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kScripts = 0xA99D8A11;
// The panel (the puck, the top bar) and its windows by id.
constexpr std::uint32_t kPanel = 0x49000000, kMainMenu = 0x00000002, kCityName = 0x00000003;
constexpr std::uint32_t kPuck = 0x4be6ed7d, kTopBar = 0x4be6ed7e, kTopBarBackground = 0x00000001;
constexpr std::uint32_t kPuckFamilies = 0x2be6f563, kPuckLots = 0x4be6f571;
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
// Tiles start 52 into the row, 95 apart; the row ends 51 past the last. A row holds at least 4 and
// as many as fit before 63 from the screen's right edge.
constexpr int kTileLeft = 52, kTileStep = 95, kTileTop = 9, kRowTail = 51, kMinTiles = 4, kScreenRightGap = 63;
// The lot catalog's picture of a lot.
constexpr std::uint32_t kImageType = 0x856DDBAC, kLotPicture = 0x35CA0002;
constexpr std::uint32_t kFamilyType = 0x46414D49, kFamilyPictureType = 0x8C3CE95A;
// Families from this id up are the game's own (townies, service people), not households.
constexpr std::uint32_t kFirstGameFamily = 0x7FE0;

// Of the windows in `window` that share an id (versions of one button), shows only the one the game
// shows: the first not hidden in the script; a sub-neighborhood chooser's second (its plus) when
// the neighborhood has none of that kind attached; the Pets version (the first) of those in `pets`.
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
        } catch (const std::exception&) {
        }
        out.push_back(std::move(picture));
    }
    return out;
}

} // namespace

NeighborhoodUi::NeighborhoodUi(ts2::ui::Screen& screen, ts2::content::UiData& data, int width, int height)
    : screen_(screen), data_(data), width_(width), height_(height) {}

void NeighborhoodUi::open(const fs::path& folder, const std::string& name, const fs::path& user) {
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
    familyPictures_ = homelessFamilies(folder);
    occupied_ = livedInLots(folder);
    layoutStrip(familyStrip_, familyPictures_);
    screen_.setVisible(families_, false);

    lots_ = screen_.load(kScripts, kLotBin, binX, binY);
    // The lots view: its background (the first of its versions), its strip and filter; the others
    // put away; Lots & Houses and All shown chosen.
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
    lotPictures_ = catalogLots(user);
    layoutStrip(lotStrip_, lotPictures_);
    screen_.setVisible(lots_, false);
    // The puck and top bar are drawn over the bins.
    screen_.raise(panel_);
    bin_ = Bin::None;
}

void NeighborhoodUi::close() {
    // Removed rather than hidden: the next neighborhood loads its own panel and bins.
    for (const int w : {panel_, families_, lots_}) {
        screen_.remove(w);
    }
    panel_ = families_ = lots_ = puck_ = -1;
    familyStrip_ = {};
    lotStrip_ = {};
    bin_ = Bin::None;
}

void NeighborhoodUi::showBin(Bin bin) {
    bin_ = bin;
    screen_.setVisible(families_, bin == Bin::Families);
    screen_.setVisible(lots_, bin == Bin::Lots);
    for (const int c : childrenWithId(screen_, puck_, kPuckFamilies)) {
        screen_.setChecked(c, bin == Bin::Families);
    }
    screen_.setChecked(screen_.find(puck_, kPuckLots), bin == Bin::Lots);
}

bool NeighborhoodUi::clicked(int window) {
    const std::uint32_t id = screen_.id(window);
    if (id == kMainMenu) {
        return true;
    }
    if (screen_.parent(window) == puck_ && id == kPuckFamilies) {
        showBin(bin_ == Bin::Families ? Bin::None : Bin::Families);
    } else if (screen_.parent(window) == puck_ && id == kPuckLots) {
        showBin(bin_ == Bin::Lots ? Bin::None : Bin::Lots);
    }
    return false;
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
