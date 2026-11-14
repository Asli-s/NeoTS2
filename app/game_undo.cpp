// Undo and redo in Buy and Build mode, and the sledgehammer. Every change made there (an object
// bought, moved or sold, floor laid, walls covered, built or taken down, a door put in, the ground
// painted) records a step that undoes it and does it again, the money with it. Ctrl+Z and the
// panels' Undo button undo the newest; Ctrl+Y and Redo do it again. The steps last until Live mode
// or another lot. Objects taken away and brought back are bought anew (with new ids, their own
// state from when they were made), so steps reach them through a cell that follows them.
#include "game.hpp"

#include "diag/log.hpp"
#include "sim/catalog.hpp"

#include <algorithm>
#include <format>

namespace app {

namespace {

// How many steps back undo reaches.
constexpr std::size_t kMostSteps = 64;

} // namespace

void Game::recordStep(std::string what, std::function<void()> undo, std::function<void()> redo) {
    undoSteps_.push_back({std::move(what), std::move(undo), std::move(redo)});
    if (undoSteps_.size() > kMostSteps) {
        undoSteps_.erase(undoSteps_.begin());
    }
    redoSteps_.clear();
}

void Game::undoStep() {
    if (undoSteps_.empty() || !lotLife_) {
        return;
    }
    cancelHeld();
    UndoStep step = std::move(undoSteps_.back());
    undoSteps_.pop_back();
    step.undo();
    ts2::diag::step("build mode: undid " + step.what);
    redoSteps_.push_back(std::move(step));
}

void Game::redoStep() {
    if (redoSteps_.empty() || !lotLife_) {
        return;
    }
    cancelHeld();
    UndoStep step = std::move(redoSteps_.back());
    redoSteps_.pop_back();
    step.redo();
    ts2::diag::step("build mode: did again " + step.what);
    undoSteps_.push_back(std::move(step));
}

void Game::clearUndo() {
    undoSteps_.clear();
    redoSteps_.clear();
    objectCells_.clear();
    pendingDoorCuts_.clear();
}

std::shared_ptr<std::int16_t> Game::cellFor(std::int16_t object) {
    for (const auto& cell : objectCells_) {
        if (*cell == object) {
            return cell;
        }
    }
    return objectCells_.emplace_back(std::make_shared<std::int16_t>(object));
}

std::int32_t Game::refundFor(std::int16_t object) {
    const auto bought = boughtThisVisit_.find(object);
    return bought != boughtThisVisit_.end() ? bought->second : lotLife_->currentValue(object);
}

std::int16_t Game::rebuy(std::uint32_t guid, const LotLife::Placed& where, std::int32_t price, bool door) {
    const std::int16_t made = lotLife_->makeForBuying(guid);
    if (made == 0) {
        ts2::diag::warnOnce(std::format("build mode: object type {:08x} couldn't be made again to undo", guid));
        return 0;
    }
    lotLife_->putBack(made, where);
    touchObject(made);
    lotLife_->addFunds(-price);
    edits_.objectFunds[made] -= price;
    if (const auto* definition = lotLife_->buyDefinition(made)) {
        lotLife_->setCurrentValue(made, ts2::sim::catalog::placedValue(*definition));
    }
    boughtThisVisit_[made] = price;
    if (door) {
        pendingDoorCuts_.emplace_back(made, 0);  // its model is drawn on a frame soon
    }
    return made;
}

void Game::takeAway(std::int16_t object, std::int32_t refund, bool door) {
    if (object == 0) {
        return;
    }
    touchObject(object);
    if (door) {
        cutForDoor(object, false);
    }
    lotLife_->addFunds(refund);
    edits_.objectFunds[object] += refund;
    boughtThisVisit_.erase(object);
    lotLife_->removeObject(object);
}

void Game::moveTo(std::int16_t object, const LotLife::Placed& where, bool door) {
    if (object == 0) {
        return;
    }
    if (door) {
        cutForDoor(object, false);
    }
    lotLife_->putBack(object, where);
    if (door) {
        cutForDoor(object, true);
    }
    touchObject(object);
}

void Game::cutPendingDoors() {
    // Tried each frame for a second or so: one whose wall is gone by then stays whole.
    constexpr int kMostFrames = 60;
    std::erase_if(pendingDoorCuts_, [&](std::pair<std::int16_t, int>& pending) {
        return !lotLife_ || ++pending.second > kMostFrames || !lotLife_->placed(pending.first) || cutForDoor(pending.first, true);
    });
}

void Game::sledgeObject(std::int16_t object) {
    if (!lotLife_ || object == 0) {
        return;
    }
    object = lotLife_->leadOf(object);
    const auto* definition = lotLife_->buyDefinition(object);
    const auto where = lotLife_->placed(object);
    if (definition == nullptr || !where) {
        return;
    }
    const std::uint32_t guid = definition->guid();
    const bool door = lotLife_->isDoorOrWindow(object);
    const std::int32_t refund = refundFor(object);
    const auto cell = cellFor(object);
    takeAway(object, refund, door);
    ts2::diag::step(std::format("build mode: sledgehammered object {} for {}", object, refund));
    recordStep(
        std::format("the sledgehammer on object {}", object),
        [this, cell, guid, where = *where, refund, door] { *cell = rebuy(guid, where, refund, door); },
        [this, cell, refund, door] { takeAway(*cell, refund, door); });
}

void Game::redrawWall(std::size_t wall) {
    if (!drawnWalls_ || wall >= drawnWalls_->walls.size() || wall >= wallInstances_.size() || !lotLife_) {
        return;
    }
    for (const bool cut : {false, true}) {
        const int instance = wallInstances_[wall][cut ? 1 : 0];
        if (instance < 0) {
            continue;
        }
        const int mesh = lotLife_->wallMesh(drawnWalls_->walls[wall], wallTopMaterial_, cut, lot_);
        if (mesh < 0) {
            continue;
        }
        lot_.instances[static_cast<std::size_t>(instance)].mesh = mesh;
        renderer_.addMaterials(lot_);
        renderer_.addMeshes(lot_);
        renderer_.setInstance(lot_, static_cast<std::size_t>(instance));
        lotPicker_.update(lot_, static_cast<std::size_t>(instance));
    }
}

Game::WallState Game::wallState() {
    WallState state;
    if (buildLot_) {
        state.graph = buildLot_->roomGraph;
        state.coverings = buildLot_->wallCoverings;
        state.patterns = buildLot_->wallPatterns;
        if (const auto rooms = buildLot_->grids.find(ts2::content::kRoomGridInstance); rooms != buildLot_->grids.end()) {
            state.rooms = rooms->second;
        }
    }
    if (drawnWalls_) {
        state.drawn = drawnWalls_->walls;
    }
    return state;
}

void Game::applyWallState(int level, const WallState& state) {
    if (!buildLot_ || !drawnWalls_ || !lotLife_) {
        return;
    }
    buildLot_->roomGraph = state.graph;
    buildLot_->wallCoverings = state.coverings;
    buildLot_->wallPatterns = state.patterns;
    if (state.rooms) {
        buildLot_->grids[ts2::content::kRoomGridInstance] = *state.rooms;
    }
    auto hide = [&](int instance) {
        if (instance < 0) {
            return;
        }
        const int mesh = lot_.instances[static_cast<std::size_t>(instance)].mesh;
        const std::size_t parts = mesh >= 0 ? lot_.meshes[static_cast<std::size_t>(mesh)].primitives.size() : 0;
        renderer_.setMaterials(static_cast<std::size_t>(instance), std::vector<int>(parts, ts2::scene::kHiddenMaterial));
    };
    // Walls built since that state go: their instances draw nothing, and their places in the scene's
    // walls look into no room.
    auto& drawn = drawnWalls_->walls;
    for (std::size_t i = state.drawn.size(); i < drawn.size(); ++i) {
        if (i < wallInstances_.size()) {
            hide(wallInstances_[i][0]);
            hide(wallInstances_[i][1]);
        }
        if (drawn[i].sceneWall >= 0) {
            lot_.walls[static_cast<std::size_t>(drawn[i].sceneWall)].roomLeft = false;
            lot_.walls[static_cast<std::size_t>(drawn[i].sceneWall)].roomRight = false;
        }
    }
    drawn = state.drawn;
    wallInstances_.resize(drawn.size(), {-1, -1});
    // Walls taken down then draw nothing; the others are drawn again, as are the storey's hubs.
    wallOfInstance_.clear();
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        for (const int instance : wallInstances_[i]) {
            if (drawn[i].removed) {
                hide(instance);
            } else if (instance >= 0) {
                wallOfInstance_[static_cast<std::size_t>(instance)] = i;
            }
        }
    }
    redrawLevelWalls(level, {}, {}, true);
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        for (const int instance : wallInstances_[i]) {
            if (!drawn[i].removed && instance >= 0) {
                wallOfInstance_[static_cast<std::size_t>(instance)] = i;
            }
        }
    }
    lotLife_->wallsChanged(*buildLot_);
}

} // namespace app
