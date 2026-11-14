// Build mode's doors, windows and arches: bought from the Doors & Windows catalog and held as Buy
// mode holds an object, one goes in a wall: it fits only where every wall its model's wall masks
// cut is built and has no other door or window in it, and it turns to face the way that fits.
// Put down, it cuts those walls (as the lot's own doors do when it is drawn) and Sims' routes go
// through it; picked up or sold, the walls close again.
#include "game.hpp"

#include "content/lot.hpp"
#include "diag/log.hpp"
#include "scene/lot_parts.hpp"
#include "sim/turn.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace app {

void Game::buildObject(std::uint32_t guid) {
    if (!lotLife_ || !buildActive_) {
        return;
    }
    dropBuildTool();
    cancelHeld();
    const auto& items = lotLife_->doorsAndWindows();
    const auto item = std::find_if(items.begin(), items.end(), [&](const ts2::sim::catalog::Item& i) { return i.guid == guid; });
    if (item == items.end()) {
        return;
    }
    if (lotLife_->funds() < item->price) {
        buildTip("You can't afford that.");
        return;
    }
    const std::int16_t made = lotLife_->makeForBuying(guid);
    if (made == 0) {
        buildTip("That can't be bought yet.");
        return;
    }
    buyHeld_ = BuyHeld{};
    buyHeld_->object = made;
    buyHeld_->fromCatalog = true;
    buyHeld_->price = item->price;
    buyHeld_->door = true;
    ts2::diag::step(std::format("build mode: holding door or window {} ({:08x})", made, guid));
    if (buildMouseAt_) {
        holdUnderMouse((*buildMouseAt_)[0], (*buildMouseAt_)[1]);
    }
}

std::optional<std::vector<Game::DoorCut>> Game::doorCuts(std::int16_t object, const LotLife::BuySpot& spot, std::int16_t direction,
                                                         bool needFree) {
    if (!drawnWalls_ || !buildLot_) {
        return std::nullopt;
    }
    if (!wallMasks_) {
        wallMasks_ = ts2::scene::wallMasks(assets_.get()->materialNames());
    }
    // The object's model, as the lot's scene draws it.
    const auto placed = std::find_if(lot_.placedObjects.begin(), lot_.placedObjects.end(),
                                     [&](const ts2::scene::PlacedObject& p) { return p.object == static_cast<std::uint32_t>(object); });
    if (placed == lot_.placedObjects.end()) {
        ts2::diag::info(std::format("build mode: object {} isn't drawn yet", object));
        return std::nullopt;
    }
    const auto masks = wallMasks_->find(ts2::scene::lower(placed->model));
    if (masks == wallMasks_->end() || masks->second.empty()) {
        ts2::diag::info("build mode: no wall masks for model " + placed->model);
        return std::nullopt;
    }
    // Placed as the scene places it: the lot's x and y are the position's y and x.
    const float x = static_cast<float>(spot.y) / 65536.0f, y = static_cast<float>(spot.x) / 65536.0f;
    const auto world = ts2::scene::Transform::at(x, y, 0, ts2::sim::compassHeading(direction));
    const int level = ts2::content::groundGridLevel(*buildLot_) + spot.level - 1;
    // A model has a mask for each face that may meet a wall. Its front face's (n) wall is the one it
    // goes in, which must be there; the others are cut where a wall happens to be.
    const auto cuts = ts2::scene::wallCuts(masks->second, world, level);
    std::vector<DoorCut> out;
    for (const auto& cut : cuts) {
        const bool needed = cut.face == "n";
        const auto& walls = drawnWalls_->walls;
        const auto key = std::pair{std::lround(cut.middle[0] * 2), std::lround(cut.middle[1] * 2)};
        const auto wall = std::find_if(walls.begin(), walls.end(), [&](const ts2::scene::DrawnWall& d) {
            return !d.removed && d.wall.level == cut.level && std::lround(d.wall.q.x0 + d.wall.q.x1) == key.first &&
                   std::lround(d.wall.q.y0 + d.wall.q.y1) == key.second;
        });
        const bool there = wall != walls.end() && wall->coverable && wall->wall.thick;
        const bool free = there && wall->wall.leftMask.empty() && wall->wall.rightMask.empty();
        if (needed && !there) {
            return std::nullopt;
        }
        if (needed && needFree && !free) {
            return std::nullopt;
        }
        if (!there || (needFree && !free)) {
            continue;
        }
        out.push_back({static_cast<std::size_t>(wall - walls.begin()),
                       ts2::scene::cutsLeftSide(cut, wall->wall.q.x0, wall->wall.q.y0, wall->wall.q.x1, wall->wall.q.y1), cut.material,
                       cut.mirrored});
    }
    // One without a front face (diagonal ones) goes in whatever wall its faces meet.
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
}

void Game::cutForDoor(std::int16_t object, bool cut) {
    if (!lotLife_ || !drawnWalls_ || !buildLot_) {
        return;
    }
    const auto placed = lotLife_->placed(object);
    if (!placed) {
        return;
    }
    const auto cuts = doorCuts(object, placed->spot, placed->direction, false);
    if (!cuts) {
        return;
    }
    const auto drawnObject = std::find_if(lot_.placedObjects.begin(), lot_.placedObjects.end(),
                                          [&](const ts2::scene::PlacedObject& p) { return p.object == static_cast<std::uint32_t>(object); });
    auto& walls = drawnWalls_->walls;
    for (const auto& c : *cuts) {
        auto& d = walls[c.wall];
        std::string& mask = c.left ? d.wall.leftMask : d.wall.rightMask;
        if (cut) {
            mask = c.material;
            (c.left ? d.wall.leftMaskMirrored : d.wall.rightMaskMirrored) = c.mirrored;
        } else if (mask == c.material) {
            mask.clear();
        } else {
            continue;  // not this one's cut
        }
        for (const bool cutDown : {false, true}) {
            const int instance = wallInstances_[c.wall][cutDown ? 1 : 0];
            if (instance < 0) {
                continue;
            }
            const int mesh = lotLife_->wallMesh(d, wallTopMaterial_, cutDown, lot_);
            if (mesh < 0) {
                continue;
            }
            lot_.instances[static_cast<std::size_t>(instance)].mesh = mesh;
            renderer_.addMaterials(lot_);
            renderer_.addMeshes(lot_);
            renderer_.setInstance(lot_, static_cast<std::size_t>(instance));
            lotPicker_.update(lot_, static_cast<std::size_t>(instance));
        }
        // The views cut the door down with its wall.
        if (drawnObject != lot_.placedObjects.end() && drawnObject->instance < lot_.instances.size() && d.sceneWall >= 0) {
            auto& inWalls = lot_.instances[drawnObject->instance].walls;
            std::erase(inWalls, d.sceneWall);
            if (cut) {
                inWalls.push_back(d.sceneWall);
            }
        }
    }
    // Sims' routes go through a door put in, and no longer through one taken out.
    lotLife_->wallsChanged(*buildLot_);
    ts2::diag::step(std::format("build mode: {} {} wall side{} for object {}", cut ? "cut" : "closed", cuts->size(), cuts->size() == 1 ? "" : "s",
                                object));
}

std::uint32_t Game::buildHover() {
    if (!buildActive_ || buildTool_ || buyHeld_ || !hoverAt_ || !lotLife_) {
        return 0;
    }
    // A door sits inside its wall's thickness: only objects take the ray, so the wall doesn't hide it.
    const std::uint32_t object = lotObjectAt((*hoverAt_)[0], (*hoverAt_)[1], {}, true);
    return object != 0 && lotLife_->isDoorOrWindow(static_cast<std::int16_t>(object)) ? object : 0;
}

} // namespace app
