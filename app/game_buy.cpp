// Buy mode on a lot: the panel opens over the paused game, a catalog item clicked is held under the
// mouse until it is put down (and paid for) or let go, and the hand picks up objects to move or sell.
// What isn't done yet: multi-tile objects (beds, sofas), objects that go on surfaces or walls, the
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
    buyActive_ = true;
    // Buy mode pauses the game; leaving it brings back the pause it had (from before the options
    // menu, when Buy mode took its place).
    pausedBeforeBuy_ = lotUi_.buyOpenedFromOptions() ? pausedBeforeOptions_ : lotLife_->paused();
    lotUi_.setBuyOpenedFromOptions(false);
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
    // Objects that go on tables, counters or walls set that in their own start-up code. Placing them
    // there isn't done yet.
    if (!lotLife_->standsOnFloor(made)) {
        lotLife_->removeObject(made);
        buyTip("Objects that go on surfaces or walls can't be placed yet.");
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
    const auto floor = lotFloorAt(x, y);
    if (!floor) {
        return;
    }
    const auto spot = lotLife_->buySpot(floor->at, floor->storey);
    if (buyHeld_->spot && *buyHeld_->spot == spot) {
        return;
    }
    buyHeld_->spot = spot;
    buyHeld_->fits = lotLife_->holdAt(buyHeld_->object, spot, buyHeld_->direction);
    buyTip(buyHeld_->fits ? std::string() : std::string("It can't go here."));
}

void Game::placeHeld() {
    if (!buyHeld_ || !lotLife_ || !buyHeld_->spot) {
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
        if (const auto* type = lotLife_->simulation().code().objectType(lotLife_->typeOf(object))) {
            lotLife_->setCurrentValue(object, ts2::sim::catalog::placedValue(type->definition));
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
        // The hand picks up the object under the mouse, when it can be moved.
        if (hovered_ != 0) {
            const auto object = static_cast<std::int16_t>(hovered_);
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
        if (buyHeld_->spot) {
            buyHeld_->fits = lotLife_->holdAt(buyHeld_->object, *buyHeld_->spot, buyHeld_->direction);
            buyTip(buyHeld_->fits ? std::string() : std::string("It can't go here."));
        }
        return true;
    }
    default: return false;
    }
}

} // namespace app
