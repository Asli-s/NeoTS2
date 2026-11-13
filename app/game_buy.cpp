// Buy mode on a lot: the panel opens over the paused game, a catalog item clicked is held under the
// mouse until it is put down (and paid for) or let go, and the hand picks up objects to move or sell.
// A multi-tile object (a bed, a sofa) is held, placed, moved and sold whole, by its lead part.
// What isn't done yet: objects that go on surfaces or walls, the
// eyedropper, sledgehammer and undo, and daily depreciation. Purchases last until the lot is left, as
// lots aren't saved yet.
#include "game.hpp"

#include "diag/log.hpp"

#include <algorithm>
#include <format>

namespace app {

namespace pf = ts2::platform;

namespace {

// The game turns a held object a quarter turn at a time (object data 1 counts eighths).
constexpr std::int16_t kQuarterTurn = 2, kTurns = 8;

} // namespace

void Game::enterBuy() {
    if (!lotLife_ || buyActive_) {
        return;
    }
    // Buy mode pauses the game; leaving it brings back the pause it had (from before the options
    // menu or Build or Story mode, when Buy mode took its place).
    pausedBeforeBuy_ = takePauseBeforeModes();
    buyActive_ = true;
    lotLife_->setPaused(true);
    boughtThisVisit_.clear();
    ts2::diag::step("buy mode: opened");
}

void Game::leaveBuy(bool restorePause) {
    if (!buyActive_) {
        return;
    }
    cancelHeld();
    buyActive_ = false;
    boughtThisVisit_.clear();
    buyTip({});
    if (restorePause && lotLife_) {
        lotLife_->setPaused(pausedBeforeBuy_);
    }
    ts2::diag::step("buy mode: closed");
}

bool Game::takePauseBeforeModes() {
    const bool fromOptions = lotUi_.modeOpenedFromOptions();
    lotUi_.setModeOpenedFromOptions(false);
    if (buyActive_) {
        const bool paused = pausedBeforeBuy_;
        leaveBuy(false);
        return paused;
    }
    if (buildActive_ || storyActive_) {
        leavePanelMode(false);
        return pausedBeforeMode_;
    }
    if (fromOptions) {
        return pausedBeforeOptions_;
    }
    return lotLife_ ? lotLife_->paused() : false;
}

void Game::enterPanelMode(bool build) {
    if (!lotLife_ || (build ? buildActive_ : storyActive_)) {
        return;
    }
    pausedBeforeMode_ = takePauseBeforeModes();
    (build ? buildActive_ : storyActive_) = true;
    lotLife_->setPaused(true);
    ts2::diag::step(build ? "build mode: opened" : "story mode: opened");
}

void Game::leavePanelMode(bool restorePause) {
    if (!buildActive_ && !storyActive_) {
        return;
    }
    dropBuildTool();
    ts2::diag::step(buildActive_ ? "build mode: closed" : "story mode: closed");
    buildActive_ = storyActive_ = false;
    if (restorePause && lotLife_) {
        lotLife_->setPaused(pausedBeforeMode_);
    }
}

void Game::buyTip(const std::string& text) {
    screen_.setSpotTip(text);
    if (!text.empty() && buyMouseAt_) {
        const auto [ux, uy] = toUi((*buyMouseAt_)[0], (*buyMouseAt_)[1]);
        screen_.moveSpotTip(ux, uy);
    }
}

void Game::buyItem(std::uint32_t guid) {
    if (!lotLife_ || !buyActive_) {
        return;
    }
    cancelHeld();
    const auto& items = lotLife_->buyItems();
    const auto item = std::find_if(items.begin(), items.end(), [&](const ts2::sim::catalog::Item& i) { return i.guid == guid; });
    if (item == items.end()) {
        return;
    }
    if (lotLife_->funds() < item->price) {
        buyTip("You can't afford that.");
        return;
    }
    const std::int16_t made = lotLife_->makeForBuying(guid);
    if (made == 0) {
        buyTip("That can't be bought yet.");
        return;
    }
    // Where an object may go (the floor, surfaces of some heights) its own start-up code sets.
    if (!lotLife_->standsOnFloor(made) && !lotLife_->goesOnSurfaces(made)) {
        lotLife_->removeObject(made);
        buyTip("That can't be placed yet.");
        return;
    }
    buyHeld_ = BuyHeld{};
    buyHeld_->object = made;
    buyHeld_->fromCatalog = true;
    buyHeld_->price = item->price;
    buyTip({});
    if (buyMouseAt_) {
        holdUnderMouse((*buyMouseAt_)[0], (*buyMouseAt_)[1]);
    }
}

void Game::holdUnderMouse(float x, float y) {
    if (!buyHeld_ || !lotLife_) {
        return;
    }
    const std::int16_t held = buyHeld_->object;
    // Over a surface (a counter, a table, a desk) with a free slot it fits, the object goes in that
    // slot. A multi-tile surface's slots are on its parts.
    if (lotLife_->goesOnSurfaces(held)) {
        const auto parts = lotLife_->partsOf(held);
        if (const std::uint32_t under = lotObjectAt(x, y, parts); under != 0) {
            for (const std::int16_t part : lotLife_->partsOf(static_cast<std::int16_t>(under))) {
                if (const auto slot = lotLife_->freeSlot(held, part)) {
                    if (buyHeld_->slot != *slot) {
                        ts2::diag::step(std::format("buy mode: object {} held in slot {} of object {}", held, slot->slot, slot->container));
                        lotLife_->holdInSlot(held, *slot);
                        buyHeld_->slot = *slot;
                        buyHeld_->spot.reset();
                    }
                    buyHeld_->fits = true;
                    buyTip({});
                    return;
                }
            }
        }
    }
    const auto floor = lotFloorAt(x, y);
    if (!floor) {
        return;
    }
    const auto spot = lotLife_->buySpot(floor->at, floor->storey);
    if (buyHeld_->spot && *buyHeld_->spot == spot && !buyHeld_->slot) {
        return;
    }
    buyHeld_->slot.reset();
    buyHeld_->spot = spot;

    // An object that only goes on surfaces is shown where the mouse is but can't be put down there.
    const bool floorAllowed = lotLife_->standsOnFloor(held);
    const bool fits = lotLife_->holdAt(held, spot, buyHeld_->direction);
    buyHeld_->fits = floorAllowed && fits;
    ts2::diag::step(std::format("buy mode: object {} held at {:.1f} {:.1f} storey {} facing {}: {}", held, spot.x / 65536.0, spot.y / 65536.0, spot.level,
                                buyHeld_->direction, buyHeld_->fits ? "fits" : "doesn't fit"));
    buyTip(buyHeld_->fits ? std::string() : floorAllowed ? std::string("It can't go here.") : std::string("It goes on a counter or table."));
}

void Game::placeHeld() {
    if (!buyHeld_ || !lotLife_ || (!buyHeld_->spot && !buyHeld_->slot)) {
        return;
    }
    if (!buyHeld_->fits) {
        buyTip("It can't go here.");
        return;
    }
    const std::int16_t object = buyHeld_->object;
    if (buyHeld_->fromCatalog) {
        if (lotLife_->funds() < buyHeld_->price) {
            buyTip("You can't afford that.");
            return;
        }
        lotLife_->addFunds(-buyHeld_->price);
        // Once down it is worth its price less its initial depreciation. Selling it before Buy mode
        // closes still gives back what it cost.
        if (const auto* definition = lotLife_->buyDefinition(object)) {
            lotLife_->setCurrentValue(object, ts2::sim::catalog::placedValue(*definition));
        }
        boughtThisVisit_[object] = buyHeld_->price;
        ts2::diag::step(std::format("buy mode: bought object {} for {}", object, buyHeld_->price));
    } else {
        ts2::diag::step(std::format("buy mode: moved object {}", object));
    }
    buyHeld_.reset();
    buyTip({});
}

void Game::cancelHeld() {
    if (!buyHeld_ || !lotLife_) {
        buyHeld_.reset();
        return;
    }
    if (buyHeld_->fromCatalog) {
        lotLife_->removeObject(buyHeld_->object);  // nothing was paid yet
    } else if (buyHeld_->from) {
        lotLife_->putBack(buyHeld_->object, *buyHeld_->from);
    }
    buyHeld_.reset();
    buyTip({});
}

void Game::sellHeld() {
    if (!buyHeld_ || !lotLife_) {
        return;
    }
    const std::int16_t object = buyHeld_->object;
    if (!buyHeld_->fromCatalog) {
        // Bought in this visit: all of it back. Otherwise what it is worth now.
        const auto bought = boughtThisVisit_.find(object);
        const std::int32_t refund = bought != boughtThisVisit_.end() ? bought->second : lotLife_->currentValue(object);
        lotLife_->addFunds(refund);
        boughtThisVisit_.erase(object);
        ts2::diag::step(std::format("buy mode: sold object {} for {}", object, refund));
    }
    lotLife_->removeObject(object);
    buyHeld_.reset();
    buyTip({});
}

bool Game::buyMouse(const pf::Event& e, float x, float y) {
    if (!buyActive_ || !lotLife_) {
        return false;
    }
    const auto [ix, iy] = toUi(x, y);
    const bool overPanels = screen_.over(ix, iy);
    if (!overPanels) {
        buyMouseAt_ = {x, y};
    }
    if (e.type == pf::Event::Type::MouseMotion && buyHeld_ && !overPanels) {
        holdUnderMouse(x, y);
        return false;  // the camera may still move
    }
    if (e.type != pf::Event::Type::MouseDown || overPanels) {
        return false;
    }
    if (e.button == pf::MouseButton::Left) {
        if (buyHeld_) {
            placeHeld();
            return true;
        }
        // The hand picks up the object under the mouse, when it can be moved: a multi-tile object as a
        // whole, by its lead.
        if (hovered_ != 0) {
            const auto object = lotLife_->leadOf(static_cast<std::int16_t>(hovered_));
            buyHeld_ = BuyHeld{};
            buyHeld_->object = object;
            buyHeld_->from = lotLife_->placed(object);
            buyHeld_->direction = buyHeld_->from ? buyHeld_->from->direction : 0;
            hovered_ = 0;
            holdUnderMouse(x, y);
            return true;
        }
    }
    return false;
}

bool Game::buyKey(const pf::Event& e) {
    if (!buyActive_ || !lotLife_ || e.type != pf::Event::Type::KeyDown) {
        return false;
    }
    if (!buyHeld_) {
        return false;
    }
    switch (e.key) {
    case pf::Key::Escape:
        if (!e.repeat) {
            cancelHeld();
        }
        return true;
    case pf::Key::Delete:
    case pf::Key::Backspace:
        if (!e.repeat) {
            sellHeld();
        }
        return true;
    case pf::Key::Comma:
    case pf::Key::Period: {
        // , and . turn what the hand holds, as they turn the camera otherwise.
        const std::int16_t step = e.key == pf::Key::Period ? kQuarterTurn : -kQuarterTurn;
        buyHeld_->direction = static_cast<std::int16_t>(((buyHeld_->direction + step) % kTurns + kTurns) % kTurns);
        // In a surface's slot it faces the surface's way; the turn shows once it is off it.
        if (buyHeld_->spot && !buyHeld_->slot) {
            buyHeld_->fits = lotLife_->holdAt(buyHeld_->object, *buyHeld_->spot, buyHeld_->direction);
            buyTip(buyHeld_->fits ? std::string() : std::string("It can't go here."));
        }
        return true;
    }
    default: return false;
    }
}

} // namespace app
