#include "game.hpp"

#include "floor_tool.hpp"
#include "placement_errors.hpp"

#include "content/floor_edit.hpp"
#include "content/terrain_edit.hpp"
#include "sim/floor_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <utility>

namespace app {

namespace {

using PlatformKind = ts2::content::BuildScripts::Platform::Kind;
constexpr int kQuarters = 4;
constexpr std::int32_t kNoPlatform = -1;
constexpr std::size_t kRegionCellSize = 16;  // four 32-bit product ids per tile
// Quarter bits: 1 the -x quarter, 2 the +y, 4 the +x and 8 the -y.
constexpr std::uint8_t kMinusX = 1, kPlusY = 2, kPlusX = 4, kMinusY = 8, kWholeTile = 0x0F;

ts2::array3d::Array3D* regionGrid(ts2::content::Lot& lot) {
    const auto found = lot.grids.find(ts2::content::kPlatformRegionGridInstance);
    return found != lot.grids.end() && found->second.elementSize == kRegionCellSize ? &found->second : nullptr;
}

std::int32_t readRegion(const ts2::array3d::Array3D& grid, std::int32_t level, std::int32_t x, std::int32_t y, int quarter) {
    if (!grid.contains(level, x, y)) {
        return kNoPlatform;
    }
    std::int32_t v = kNoPlatform;
    std::memcpy(&v, grid.at(level, x, y).data() + static_cast<std::size_t>(quarter) * sizeof v, sizeof v);
    return v;
}

void writeRegion(ts2::array3d::Array3D& grid, std::int32_t level, std::int32_t x, std::int32_t y, int quarter, std::int32_t v) {
    if (grid.contains(level, x, y)) {
        std::memcpy(grid.at(level, x, y).data() + static_cast<std::size_t>(quarter) * sizeof v, &v, sizeof v);
    }
}

// The product the quarters in `mask` share, none (-1) when they differ, have none or are off the grid.
std::int32_t sharedRegion(const ts2::array3d::Array3D& grid, std::int32_t level, std::int32_t x, std::int32_t y, std::uint8_t mask) {
    std::optional<std::int32_t> shared;
    for (int q = 0; q < kQuarters; ++q) {
        if ((mask & (1u << q)) == 0) {
            continue;
        }
        const std::int32_t v = readRegion(grid, level, x, y, q);
        if (shared && *shared != v) {
            return kNoPlatform;
        }
        shared = v;
    }
    return shared.value_or(kNoPlatform);
}

// The tile quarters either side of a wall segment within one tile: the tile edge's two neighbours,
// or the two halves a diagonal cuts a tile into.
struct Side {
    std::int32_t x = 0, y = 0;
    std::uint8_t mask = 0;
};
std::array<Side, 2> sidesOf(const ts2::content::WallSegment& s) {
    const auto x0 = static_cast<std::int32_t>(std::floor(std::min(s.ax, s.bx)));
    const auto y0 = static_cast<std::int32_t>(std::floor(std::min(s.ay, s.by)));
    if (s.ay == s.by) {
        return {{{x0, y0 - 1, kPlusY}, {x0, y0, kMinusY}}};
    }
    if (s.ax == s.bx) {
        return {{{x0 - 1, y0, kPlusX}, {x0, y0, kMinusX}}};
    }
    const bool rising = (s.bx - s.ax) * (s.by - s.ay) > 0;
    return rising ? std::array<Side, 2>{{{x0, y0, kMinusY | kPlusX}, {x0, y0, kMinusX | kPlusY}}}
                  : std::array<Side, 2>{{{x0, y0, kMinusX | kMinusY}, {x0, y0, kPlusY | kPlusX}}};
}

// Both pool products count as one platform when the game compares the sides of a wall.
std::int32_t sameKind(std::int32_t product) {
    constexpr std::int32_t kPool = 3, kOtherPool = 5;
    return product == kOtherPool ? kPool : product;
}

} // namespace

bool Game::smashPlatform(std::int32_t level, std::int32_t x, std::int32_t y) {
    if (!lotLife_ || !lotContent_) {
        return false;
    }
    auto* grid = regionGrid(*lotContent_);
    if (grid == nullptr) {
        return false;
    }
    const auto& scripts = assets_.get()->buildScripts();
    const std::int32_t ground = ts2::content::groundGridLevel(*lotContent_);
    const std::int32_t below = level - 1;
    const auto platformOf = [&](std::int32_t product) {
        const auto found = scripts.platforms.find(product);
        return found != scripts.platforms.end() ? &found->second : nullptr;
    };
    // A foundation stands only on the ground storey, a deck on any. Stages, and decks laid out with
    // support columns, are not done yet.
    const auto* platform = below >= ground ? platformOf(sharedRegion(*grid, below, x, y, kWholeTile)) : nullptr;
    if (platform == nullptr || !((platform->kind == PlatformKind::Foundation && below == ground) || platform->kind == PlatformKind::Deck) ||
        platform->laysSupports) {
        return false;
    }
    startWallManager();
    stopFloorTool();
    SmashedPlatform smashed;
    float cost = 0;
    constexpr float kSellBack = 0.8f;

    // The tile's four quarters of the platform go, each giving back a quarter of four fifths of the
    // platform's price.
    for (int q = 0; q < kQuarters; ++q) {
        const std::int32_t was = readRegion(*grid, below, x, y, q);
        if (was == kNoPlatform) {
            continue;
        }
        writeRegion(*grid, below, x, y, q, kNoPlatform);
        smashed.regions.push_back({below, x, y, q, was});
        cost -= std::nearbyint(static_cast<float>(lotUi_.platformPrice(was)) * kSellBack) * 0.25f;
    }

    // The floor on top goes with it, unless an object on the tile keeps it: on the ground storey one
    // without object data 0x2A bit 0x2, above it one without bit 0x200.
    constexpr std::uint16_t kGroundFloorFree = 0x2, kUpperFloorFree = 0x200;
    const std::int32_t storey = level - ground;
    const auto objects = terrainObjects(x, y, storey);
    const bool kept = std::any_of(objects.begin(), objects.end(), [&](const ts2::content::TerrainObject& o) {
        return storey == 0 ? (o.placement & kGroundFloorFree) == 0 : (o.placement & kUpperFloorFree) == 0;
    });
    if (!kept) {
        for (int q = 0; q < kQuarters; ++q) {
            if (!ts2::content::floorPatternAt(*lotContent_, level, x, y, q).empty()) {
                smashed.floorsAfter.push_back({level, x, y, q, {}, 1});
            }
        }
        cost += app::floorStrokeCost(*lotContent_, smashed.floorsAfter, app::FloorStroke::Remove,
                                     [&](const std::string& name) { return lotUi_.floorPrice(name); });
        smashed.floorsBefore = ts2::content::setFloors(*lotContent_, smashed.floorsAfter);
    }

    // Sides on the tile's edges whose two sides are now the same come out with the wall tool's checks.
    // A side of a platform still standing beside the tile has the hole on its other side, so it stays.
    const std::int32_t wallLevel = below - ground;
    const float x0 = static_cast<float>(x), y0 = static_cast<float>(y), x1 = x0 + 1, y1 = y0 + 1;
    const auto inTile = [&](float px, float py) { return px >= x0 && px <= x1 && py >= y0 && py <= y1; };
    const auto regionBeside = [&](const Side& side, bool stagesCount) {
        const std::int32_t product = sharedRegion(*grid, below, side.x, side.y, side.mask);
        const auto* p = platformOf(product);
        return !stagesCount && p != nullptr && p->kind == PlatformKind::Stage ? kNoPlatform : sameKind(product);
    };
    std::vector<ts2::content::WallSegment> onTile;
    for (const auto& [cx, cy] : {std::pair{x0, y0}, std::pair{x1, y0}, std::pair{x0, y1}, std::pair{x1, y1}}) {
        for (const auto& w : wallManager_->wallsAt(cx, cy, wallLevel)) {
            const auto& s = w.segment;
            const bool known = std::any_of(onTile.begin(), onTile.end(), [&](const auto& o) {
                return (o.ax == s.ax && o.ay == s.ay && o.bx == s.bx && o.by == s.by) || (o.ax == s.bx && o.ay == s.by && o.bx == s.ax && o.by == s.ay);
            });
            if (!known && inTile(s.ax, s.ay) && inTile(s.bx, s.by)) {
                onTile.push_back(s);
            }
        }
    }
    std::optional<int> refused;
    for (const auto& s : onTile) {
        const auto wall = wallManager_->keptOn(s);
        const auto sides = sidesOf(s);
        if (!wall || regionBeside(sides[0], false) != regionBeside(sides[1], false)) {
            continue;
        }
        const auto style = scripts.wallStyles.find(wall->style);
        if (style == scripts.wallStyles.end() || (style->second.deleteTools & ts2::content::WallStyle::kDeleteLevelRoom) == 0) {
            continue;
        }
        std::vector<std::uint32_t> hung;
        if (const auto why = demolishRefusals(wall->segment, hung); !why.empty()) {
            refused = why.front();
            break;
        }
        if (!wallManager_->remove(wall->segment)) {
            continue;
        }
        smashed.wallsRemoved.push_back({wall->segment, wall->style, wall->left, wall->right});
        cost -= std::trunc(static_cast<float>(lotUi_.wallStylePrice(wall->style)) * kSellBack);
        for (const std::uint32_t object : hung) {
            if (std::none_of(smashed.sold.begin(), smashed.sold.end(), [&](const auto& done) { return done.first == object; })) {
                if (const auto value = sellWithWall(object)) {
                    smashed.sold.push_back({object, *value});
                    cost -= static_cast<float>(*value);
                }
            }
        }
    }
    if (refused) {
        // The whole change is undone and the game says why.
        for (const auto& [object, value] : smashed.sold) {
            lotLife_->handRestore(object, 0);
        }
        for (auto it = smashed.wallsRemoved.rbegin(); it != smashed.wallsRemoved.rend(); ++it) {
            wallManager_->add(it->style, it->left, it->right, it->segment);
        }
        ts2::content::setFloors(*lotContent_, smashed.floorsBefore);
        for (const auto& r : smashed.regions) {
            writeRegion(*grid, r.level, r.x, r.y, r.quarter, r.before);
        }
        constexpr std::uint32_t kUiText = 0x7FA7DC7C;  // string id, leakcheck: ok
        const std::string text = app::placementErrorText(
            [&](std::uint32_t table, std::uint32_t line) { return screen_.data().text(kUiText, table << 16 | line); }, *refused, 0, 0);
        if (!text.empty()) {
            showWorldTip(text, lotLife_->scenePointAt(y0 + 0.5f, x0 + 0.5f, storey + 1, lot_));
        }
        if (audio_) {
            (void)audio_->playUi("ui_error");
        }
        return true;
    }

    // Each edge between the hole and a platform still standing gets that platform's side, unless a
    // wall stands there.
    const std::array<ts2::content::WallSegment, 4> edges{{{x0, y0, x1, y0, wallLevel},
                                                          {x0, y1, x1, y1, wallLevel},
                                                          {x0, y0, x0, y1, wallLevel},
                                                          {x1, y0, x1, y1, wallLevel}}};
    for (const auto& edge : edges) {
        const auto sides = sidesOf(edge);
        const std::int32_t left = regionBeside(sides[0], true), right = regionBeside(sides[1], true);
        if (left == right) {
            continue;
        }
        const auto* beside = platformOf(left != kNoPlatform ? left : right);
        if (beside == nullptr || wallManager_->has(edge)) {
            continue;
        }
        const auto style = scripts.wallStyles.find(beside->wallStyle);
        const std::string pattern = style != scripts.wallStyles.end() ? style->second.defaultPattern : std::string();
        if (wallManager_->add(beside->wallStyle, pattern, pattern, edge)) {
            smashed.wallsAdded.push_back({edge, beside->wallStyle, pattern, pattern});
        }
    }
    wallManager_->save();

    // The corners round the tile on the storey that was its top go back to a storey above the corners
    // under them, where the game's check allows. Corners of tiles that still hold a floor up keep
    // their height.
    {
        const auto support = app::floorSupport(*lotContent_, *assets_.get());
        const auto bounds = lotContent_->worldBounds.value_or(ts2::content::WorldBounds{});
        ts2::content::TerrainEdit edit(*lotContent_, *wallManager_, scripts,
                                       [this](std::int32_t tx, std::int32_t ty, std::int32_t l) { return terrainObjects(tx, ty, l); },
                                       smashed.heights, lotLife_->simulation().world().lotShape().beachSide);
        edit.setFloorAllowed([&](const ts2::content::TerrainCorner& t) { return support.allowed(t.level + ground, t.x - bounds.minX, t.y - bounds.minY); });
        for (std::int32_t cx = x - 1; cx <= x + 2; ++cx) {
            for (std::int32_t cy = y - 1; cy <= y + 2; ++cy) {
                edit.platformRemoved({cx + bounds.minX, cy + bounds.minY, storey});
            }
        }
    }

    platformsSmashed_.push_back(std::move(smashed));
    const auto charged = buildFree() ? 0 : static_cast<std::int32_t>(cost);
    buyHand_.recordPlatform(static_cast<std::uint32_t>(platformsSmashed_.size() - 1), charged);
    lotLife_->handPay(charged);
    if (charged != 0) {
        moneyText_.show(screen_, charged, lotMouseUi_[0], lotMouseUi_[1]);
    }
    if (audio_) {
        (void)audio_->playUi("ui_object_sell");
    }
    floorBlocked_.clear();
    wallsChanged();
    floorsChanged();
    return true;
}

void Game::togglePlatform(std::uint32_t id, bool in) {
    if (!lotContent_ || id >= platformsSmashed_.size()) {
        return;
    }
    auto* grid = regionGrid(*lotContent_);
    if (grid == nullptr) {
        return;
    }
    startWallManager();
    stopFloorTool();
    auto& smashed = platformsSmashed_[id];
    // An undo goes from the last change to the first, a redo the other way.
    if (!in) {
        smashed.heights.toggle(*lotContent_);
        for (auto it = smashed.wallsAdded.rbegin(); it != smashed.wallsAdded.rend(); ++it) {
            wallManager_->remove(it->segment);
        }
        for (auto it = smashed.wallsRemoved.rbegin(); it != smashed.wallsRemoved.rend(); ++it) {
            wallManager_->add(it->style, it->left, it->right, it->segment);
        }
        for (const auto& [object, value] : smashed.sold) {
            lotLife_->handRestore(object, 0);
        }
        ts2::content::setFloors(*lotContent_, smashed.floorsBefore);
        for (const auto& r : smashed.regions) {
            writeRegion(*grid, r.level, r.x, r.y, r.quarter, r.before);
        }
    } else {
        for (const auto& r : smashed.regions) {
            writeRegion(*grid, r.level, r.x, r.y, r.quarter, kNoPlatform);
        }
        ts2::content::setFloors(*lotContent_, smashed.floorsAfter);
        for (const auto& [object, value] : smashed.sold) {
            lotLife_->handKeep(object, 0);
        }
        for (const auto& w : smashed.wallsRemoved) {
            wallManager_->remove(w.segment);
        }
        for (const auto& w : smashed.wallsAdded) {
            wallManager_->add(w.style, w.left, w.right, w.segment);
        }
        smashed.heights.toggle(*lotContent_);
    }
    floorBlocked_.clear();
    wallsChanged();
    floorsChanged();
}

} // namespace app
