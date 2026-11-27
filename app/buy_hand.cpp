#include "buy_hand.hpp"

#include "scene/lot_parts.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

namespace app {

std::optional<float> groundHit(const ts2::scene::Scene& scene, const ts2::content::Lot& lot, const std::array<float, 3>& eye,
                               const std::array<float, 3>& ray, bool poolTops) {
    const auto terrain = std::find_if(scene.instances.begin(), scene.instances.end(), [](const ts2::scene::Instance& i) {
        return i.object == 0 && !i.neighborHouse && !i.imposterLot && i.name == "terrain";
    });
    const ts2::scene::Heights heights(lot);
    const auto* grid = heights.grid();
    if (terrain == scene.instances.end() || grid == nullptr || grid->width < 2 || grid->height < 2) {
        return std::nullopt;
    }
    const int level = ts2::content::groundGridLevel(lot);
    const auto surface = lot.grids.find(ts2::content::kSurfaceGridInstance);
    // Into the ground's own space, where its corners stand at whole x and y.
    const auto toGround = terrain->transform.rigidInverse();
    std::array<float, 3> o = eye, d = ray;
    toGround.apply(o.data(), true);
    toGround.apply(d.data(), false);
    const float width = static_cast<float>(grid->width - 1), depth = static_cast<float>(grid->height - 1);
    const auto above = [&](float t) { return o[2] + d[2] * t - heights.blended(level, o[0] + d[0] * t, o[1] + d[1] * t); };
    const auto open = [&](float t) {
        return !poolTops && surface != lot.grids.end() && ts2::scene::surfaceOpen(surface->second, level, o[0] + d[0] * t, o[1] + d[1] * t);
    };
    float t0 = 0, t1 = std::numeric_limits<float>::max();
    for (const auto& [origin, dir, high] : {std::tuple{o[0], d[0], width}, std::tuple{o[1], d[1], depth}}) {
        if (std::abs(dir) < 1e-9f) {
            if (origin < 0 || origin > high) {
                return std::nullopt;
            }
            continue;
        }
        const float a = (0 - origin) / dir, b = (high - origin) / dir;
        t0 = std::max(t0, std::min(a, b));
        t1 = std::min(t1, std::max(a, b));
    }
    if (t0 > t1) {
        return std::nullopt;
    }
    constexpr float kStep = 0.25f;  // tiles
    const float across = std::sqrt(d[0] * d[0] + d[1] * d[1]);
    if (across <= 1e-6f) {
        // Straight down or up: the ground is right below the eye.
        if (std::abs(d[2]) < 1e-9f) {
            return std::nullopt;
        }
        const float t = (heights.blended(level, o[0], o[1]) - o[2]) / d[2];
        return t >= 0 && !open(t) ? std::optional<float>(t) : std::nullopt;
    }
    const float dt = kStep / across;
    float before = t0;
    if (above(before) <= 0 && !open(before)) {
        return before;
    }
    const auto steps = static_cast<int>(std::min(1e6f, std::ceil((t1 - t0) / dt))) + 1;
    for (int step = 1; step <= steps; ++step) {
        const float at = std::min(t0 + static_cast<float>(step) * dt, t1);
        if (above(at) <= 0) {
            float lo = before, hi = at;
            for (int k = 0; k < 16; ++k) {
                const float mid = (lo + hi) / 2;
                (above(mid) <= 0 ? hi : lo) = mid;
            }
            if (!open(hi)) {
                return hi;
            }
        }
        before = at;
    }
    return std::nullopt;
}

std::optional<BuyHand::FloorPoint> BuyHand::floorPoint(const ts2::scene::Scene& scene, std::span<const std::uint8_t> shown,
                                                       const ts2::scene::Picker& picker, const std::array<float, 3>& eye,
                                                       const std::array<float, 3>& ray, int groundLevel, const ts2::content::Lot* lot,
                                                       bool poolTops) {
    // The storeys' tile grids count as floor: over a flower bed beside a foundation the point is on
    // the foundation storey's grid, where the object can't stand.
    const auto floorOrGround = [lot](const ts2::scene::Instance& instance) {
        using ts2::scene::Part;
        return instance.object == 0 && ((lot == nullptr && instance.name == "terrain") || (instance.part == Part::Floor && instance.name != "pool water") ||
                                        instance.part == Part::UpperGrid || instance.part == Part::ActiveGrid);
    };
    const auto hit = picker.pickAmong(scene, shown, eye, ray, floorOrGround);
    const auto ground = lot != nullptr ? groundHit(scene, *lot, eye, ray, poolTops) : std::nullopt;
    if (!hit && !ground) {
        return std::nullopt;
    }
    const bool onGround = ground && (!hit || *ground < hit->distance);
    const float distance = onGround ? *ground : hit->distance;
    const std::array<float, 3> at{eye[0] + ray[0] * distance, eye[1] + ray[1] * distance, eye[2] + ray[2] * distance};
    return FloorPoint{at, onGround || scene.instances[hit->instance].name == "terrain" ? 1 : scene.instances[hit->instance].level - groundLevel + 1};
}

void BuyHand::setBad(LotLife& lot, std::optional<ts2::sim::LotSimulation::PlaceRefusal> refusal) {
    const bool bad = refusal.has_value();
    refusal_ = refusal;
    bad_ = bad;
    lot.setHandRefused(bad ? carried_ : 0);
    lot.setHandMarker(carried_, !bad);
}

void BuyHand::release(LotLife& lot) {
    setBad(lot, std::nullopt);
    lot.setHandMarker(0, true);
    carried_ = 0;
    buying_ = 0;
    from_.reset();
    lifted_ = 0;
    stashed_ = false;
    lastTile_.reset();
    point_.reset();
}

void BuyHand::retest(LotLife& lot, std::int16_t facing, bool turnToWalls) {
    // A new catalog object stands off the lot until the mouse first moves over it.
    auto spot = point_ ? std::optional{lot.handSpotAt(point_->point, point_->storey, facing)} : lot.handSpotOf(carried_);
    if (!spot) {
        return;
    }
    spot->facing = facing;
    // A quarter turn at a time. When no facing fits, the item's other version (a door's diagonal one
    // over a diagonal wall, or its straight one again) tries the same facings and takes the hand's
    // place if one fits. A lifted object waits aside where it stood while its other version is held.
    if (turnToWalls && lot.handTurnsToWalls(carried_)) {
        constexpr std::int16_t kQuarter = 2, kFacings = 8;
        const auto firstFit = [&](std::uint32_t object) -> std::optional<LotLife::HandSpot> {
            auto tried = *spot;
            for (int i = 0; i < kFacings / kQuarter; ++i, tried.facing = static_cast<std::int16_t>((tried.facing + kQuarter) % kFacings)) {
                if (lot.handFits(object, tried)) {
                    return tried;
                }
            }
            return std::nullopt;
        };
        auto fit = firstFit(carried_);
        if (!fit && lifted_ != 0) {
            const std::uint32_t standIn = carried_;
            if (lot.handKeep(standIn, 0) && lot.handRestore(lifted_, 0)) {
                if ((fit = firstFit(lifted_))) {
                    (void)lot.handRestore(standIn, 0);
                    lot.handDrop(standIn);
                    carried_ = std::exchange(lifted_, 0);
                } else {
                    (void)lot.handKeep(lifted_, 0);
                    (void)lot.handRestore(standIn, 0);
                }
            }
        } else if (!fit && buying_ != 0) {
            const auto other = lot.otherVersion(buying_);
            if (const auto made = other ? lot.handCreate(*other) : std::nullopt) {
                lot.handRecolour(*made, colours_);
                // The copy held, one tile back, mustn't stand in the other version's way.
                const bool aside = lot.handKeep(carried_, 0);
                fit = firstFit(*made);
                if (aside) {
                    (void)lot.handRestore(carried_, 0);
                }
                if (fit) {
                    lot.handDrop(carried_);
                    carried_ = *made;
                    buying_ = *other;
                } else {
                    lot.handDrop(*made);
                }
            }
        } else if (!fit && from_) {
            const auto sale = lot.handSale(carried_);
            const auto other = sale ? lot.otherVersion(sale->guid) : std::nullopt;
            if (const auto made = other ? lot.handCreate(*other) : std::nullopt) {
                // The copy wears the lifted object's colours.
                lot.handRecolour(*made, lot.eyedropColours(carried_));
                lot.handMove(carried_, *from_);
                if (lot.handKeep(carried_, 0)) {
                    if ((fit = firstFit(*made))) {
                        lifted_ = std::exchange(carried_, *made);
                    } else {
                        (void)lot.handRestore(carried_, 0);
                        lot.handDrop(*made);
                    }
                } else {
                    lot.handDrop(*made);
                }
            }
        }
        if (fit) {
            spot = fit;
            facing = fit->facing;
        }
    }
    setBad(lot, lot.handPlaceRefusal(carried_, *spot));
    lot.handMove(carried_, bad_ && point_ ? lot.handSpotAt(point_->point, point_->storey, facing, false) : *spot);
}

// A new object of the catalog item for the hand, at `at`, or off the lot until the mouse moves over
// it when none.
void BuyHand::takeNew(LotLife& lot, std::optional<LotLife::HandSpot> at) {
    const std::uint32_t guid = buying_;
    const auto made = lot.handCreate(guid);
    release(lot);
    if (!made) {
        return;
    }
    carried_ = *made;
    buying_ = guid;
    lot.handRecolour(carried_, colours_);
    if (at) {
        lot.handMove(carried_, *at);
        lastTile_ = std::array<int, 3>{static_cast<int>(std::floor(at->x)), static_cast<int>(std::floor(at->y)), at->storey};
        setBad(lot, lot.handPlaceRefusal(carried_, *at));
    } else {
        setBad(lot, std::nullopt);
    }
}

// The game checks the price against the funds as the item is put down, and refuses it when they
// don't cover it.
BuyHand::Paid BuyHand::click(LotLife& lot, std::uint32_t object) {
    if (carried_ != 0) {
        const auto to = lot.handSpotOf(carried_);
        if (bad_ && refusal_) {
            return {Paid::What::Refused, 0, refusal_->code, carried_, refusal_};
        }
        if (bad_ || !to) {
            return {};
        }
        const std::uint32_t sound = lot.handPutDownSound(carried_);
        if (buying_ != 0) {
            const std::int32_t cost = lot.handPrice(carried_);
            if (cost > 0 && cost > lot.funds()) {
                return {Paid::What::CantAfford, cost};
            }
            record({Change::Kind::Purchase, carried_, *to, *to, cost});
            lot.handPay(cost);
            // Build mode, Shift held in Buy mode, or the item itself (chairs, for one) gives the hand
            // another to place.
            if (lot.handRepeats(carried_, buying_, inBuild_ || ts2::platform::keyHeld(ts2::platform::Key::Shift))) {
                takeNew(lot, to);
            } else {
                release(lot);
            }
            return {Paid::What::Bought, cost, 0, 0, std::nullopt, sound};
        }
        if (from_ && lifted_ != 0) {
            Change swap{Change::Kind::Swap, carried_, *from_, *to, 0};
            swap.replaced = lifted_;
            record(swap);
        } else if (from_) {
            record({Change::Kind::Move, carried_, *from_, *to, 0});
        }
        release(lot);
        return {Paid::What::PutDown, 0, 0, 0, std::nullopt, sound};
    }
    if (object == 0) {
        return {};
    }
    if (const auto refusal = lot.handRefusal(object)) {
        return refusal->code != 0 ? Paid{Paid::What::Refused, 0, refusal->code, refusal->at} : Paid{};
    }
    carried_ = lot.handLead(object);
    from_ = lot.handSpotOf(carried_);
    lastTile_.reset();
    setBad(lot, std::nullopt);
    return {Paid::What::Lifted};
}

void BuyHand::buy(LotLife& lot, std::uint32_t guid, std::vector<ts2::content::RecolourChoice> colours) {
    cancel(lot);
    buying_ = guid;
    colours_ = std::move(colours);
    takeNew(lot, std::nullopt);
}

void BuyHand::recolour(LotLife& lot, std::vector<ts2::content::RecolourChoice> colours) {
    if (buying_ == 0 || carried_ == 0) {
        return;
    }
    colours_ = std::move(colours);
    const auto at = lot.handSpotOf(carried_);
    lot.handDrop(carried_);
    takeNew(lot, at);
}

void BuyHand::cancel(LotLife& lot) {
    if (carried_ == 0) {
        release(lot);
        return;
    }
    unswap(lot);
    if (buying_ != 0) {
        lot.handDrop(carried_);
    } else if (from_) {
        lot.handMove(carried_, *from_);
    }
    release(lot);
}

void BuyHand::stash(LotLife& lot, bool down) {
    if (down && !stashed_ && carried_ != 0 && buying_ != 0 && lot.isDoorOrWindow(buying_)) {
        const std::uint32_t guid = buying_;
        const auto at = lot.handSpotOf(carried_);
        lot.handDrop(carried_);
        release(lot);
        buying_ = guid;
        stashed_ = true;
        stashedAt_ = at;
    } else if (!down && stashed_) {
        stashed_ = false;
        takeNew(lot, stashedAt_);
    }
}

void BuyHand::unswap(LotLife& lot) {
    if (lifted_ != 0 && lot.handRestore(lifted_, 0)) {
        lot.handDrop(carried_);
        carried_ = std::exchange(lifted_, 0);
    }
}

std::optional<std::int32_t> BuyHand::sell(LotLife& lot) {
    if (carried_ == 0) {
        return std::nullopt;
    }
    // A catalog item not bought yet is only dropped.
    if (buying_ != 0) {
        cancel(lot);
        return std::nullopt;
    }
    unswap(lot);
    const auto to = lot.handSpotOf(carried_);
    const auto sold = lot.handSell(carried_);
    if (!sold) {
        return std::nullopt;
    }
    if (from_ && to) {
        record({Change::Kind::Sale, carried_, *from_, *to, *sold});
    }
    release(lot);
    return sold;
}

std::optional<std::int32_t> BuyHand::smash(LotLife& lot, std::uint32_t object) {
    if (carried_ != 0) {
        return std::nullopt;
    }
    const std::uint32_t lead = lot.handLead(object);
    const auto at = lot.handSpotOf(lead);
    const auto sold = at ? lot.handSell(lead) : std::nullopt;
    if (sold) {
        record({Change::Kind::Sale, lead, *at, *at, *sold});
    }
    return sold;
}

void BuyHand::recordRecolour(std::uint32_t object, std::vector<ts2::content::RecolourChoice> before,
                             std::vector<ts2::content::RecolourChoice> after, std::int32_t cost) {
    Change change;
    change.kind = Change::Kind::Recolour;
    change.object = object;
    change.money = cost;
    change.before = std::move(before);
    change.after = std::move(after);
    record(change);
}

void BuyHand::recordWalls(std::uint32_t id, std::int32_t cost) {
    Change change;
    change.kind = Change::Kind::Walls;
    change.object = id;
    change.money = cost;
    record(change);
}

void BuyHand::recordFloors(std::uint32_t id, std::int32_t cost) {
    Change change;
    change.kind = Change::Kind::Floors;
    change.object = id;
    change.money = cost;
    record(change);
}

void BuyHand::recordPlatform(std::uint32_t id, std::int32_t cost) {
    Change change;
    change.kind = Change::Kind::Platform;
    change.object = id;
    change.money = cost;
    record(change);
}

void BuyHand::recordPaint(std::uint32_t id, std::int32_t cost) {
    Change change;
    change.kind = Change::Kind::Paint;
    change.object = id;
    change.money = cost;
    record(change);
}

void BuyHand::record(const Change& change) {
    undo_.push_back(change);
    redo_.clear();
}

bool BuyHand::undo(LotLife& lot) {
    if ((carried_ != 0 && buying_ == 0) || undo_.empty()) {
        return false;
    }
    const Change change = undo_.back();
    undo_.pop_back();
    // A change that can't be taken back ends the history, as in the game.
    bool done = true;
    switch (change.kind) {
    case Change::Kind::Move: lot.handMove(change.object, change.from); break;
    case Change::Kind::Sale:
        done = lot.handRestore(change.object, -change.money);
        if (done) {
            lot.handMove(change.object, change.from);
        }
        break;
    case Change::Kind::Purchase: done = lot.handKeep(change.object, change.money); break;
    case Change::Kind::Swap: done = lot.handKeep(change.object, 0) && lot.handRestore(change.replaced, 0); break;
    case Change::Kind::Recolour:
        lot.handRecolour(change.object, change.before);
        lot.handPay(-change.money);
        break;
    case Change::Kind::Walls:
        if (walls_) {
            walls_(change.object, false);
        }
        lot.handPay(-change.money);
        break;
    case Change::Kind::Floors:
        if (floors_) {
            floors_(change.object, false);
        }
        lot.handPay(-change.money);
        break;
    case Change::Kind::Platform:
        if (platforms_) {
            platforms_(change.object, false);
        }
        lot.handPay(-change.money);
        break;
    case Change::Kind::Paint:
        if (paint_) {
            paint_(change.object, false);
        }
        lot.handPay(-change.money);
        break;
    }
    if (!done) {
        undo_.clear();
        return false;
    }
    redo_.push_back(change);
    // The change may have filled or freed the spot under a held catalog item.
    if (carried_ != 0) {
        retest(lot, lot.handFacing(carried_));
    }
    return true;
}

bool BuyHand::redo(LotLife& lot) {
    if ((carried_ != 0 && buying_ == 0) || redo_.empty()) {
        return false;
    }
    const Change change = redo_.back();
    redo_.pop_back();
    bool done = true;
    switch (change.kind) {
    case Change::Kind::Move: lot.handMove(change.object, change.to); break;
    case Change::Kind::Sale:
        lot.handMove(change.object, change.to);
        done = lot.handSell(change.object).has_value();
        break;
    case Change::Kind::Purchase: done = lot.handRestore(change.object, -change.money); break;
    case Change::Kind::Swap: done = lot.handKeep(change.replaced, 0) && lot.handRestore(change.object, 0); break;
    case Change::Kind::Recolour:
        lot.handRecolour(change.object, change.after);
        lot.handPay(change.money);
        break;
    case Change::Kind::Walls:
        if (walls_) {
            walls_(change.object, true);
        }
        lot.handPay(change.money);
        break;
    case Change::Kind::Floors:
        if (floors_) {
            floors_(change.object, true);
        }
        lot.handPay(change.money);
        break;
    case Change::Kind::Platform:
        if (platforms_) {
            platforms_(change.object, true);
        }
        lot.handPay(change.money);
        break;
    case Change::Kind::Paint:
        if (paint_) {
            paint_(change.object, true);
        }
        lot.handPay(change.money);
        break;
    }
    if (!done) {
        redo_.clear();
        return false;
    }
    undo_.push_back(change);
    if (carried_ != 0) {
        retest(lot, lot.handFacing(carried_));
    }
    return true;
}

void BuyHand::clearHistory(LotLife& lot) {
    undo_.clear();
    redo_.clear();
    lot.handFinishSales();
}

void BuyHand::moved(LotLife& lot, const FloorPoint& at) {
    if (carried_ == 0) {
        return;
    }
    point_ = at;
    const std::int16_t facing = lot.handFacing(carried_);
    const LotLife::HandSpot spot = lot.handSpotAt(at.point, at.storey, facing);
    // The game tests the object again only when the tile under the mouse changes. Where it can't
    // stand, it is shown at the mouse itself, not snapped to the tile.
    const std::array<int, 3> tile{static_cast<int>(std::floor(spot.x)), static_cast<int>(std::floor(spot.y)), spot.storey};
    const bool newTile = lastTile_ != tile;
    if (newTile && lot.handTurnsToWalls(carried_)) {
        lastTile_ = tile;
        retest(lot, facing, true);
        return;
    }
    if (newTile) {
        lastTile_ = tile;
        setBad(lot, lot.handPlaceRefusal(carried_, spot));
    }
    if (bad_) {
        lot.handMove(carried_, lot.handSpotAt(at.point, at.storey, facing, false));
    } else if (newTile) {
        lot.handMove(carried_, spot);
    }
}

void BuyHand::face(LotLife& lot, std::int16_t facing) {
    if (carried_ != 0 && lot.handFacing(carried_) != facing) {
        retest(lot, facing, true);
    }
}

std::optional<BuyHand::DragTurn> BuyHand::dragTurn(int dx, int dy, const std::function<std::array<float, 2>(std::int16_t)>& front) {
    constexpr int kSlack = 16;
    constexpr float kPi = 3.14159265f;
    if (std::abs(dx) + std::abs(dy) <= kSlack) {
        return std::nullopt;
    }
    // Screen y goes down.
    const float v = -std::atan2(static_cast<float>(dy), static_cast<float>(dx)) * 2.0f / kPi;
    const int quarter = ((static_cast<int>(std::floor(v)) % 4) + 4) % 4;
    // The facing whose front on screen is nearest the middle of that quarter.
    const float middle = -(static_cast<float>(quarter) + 0.5f) * kPi / 2.0f;
    DragTurn best{0, quarter};
    float nearest = 10.0f;
    for (std::int16_t facing = 0; facing < 8; facing += 2) {
        const auto [fx, fy] = front(facing);
        float off = std::atan2(fy, fx) - middle;
        off = std::abs(std::remainder(off, 2.0f * kPi));
        if (off < nearest) {
            nearest = off;
            best.facing = facing;
        }
    }
    return best;
}

void BuyHand::turn(LotLife& lot, int way) {
    constexpr int kQuarter = 2, kFacings = 8;
    if (carried_ != 0) {
        retest(lot, static_cast<std::int16_t>(((lot.handFacing(carried_) + way * kQuarter) % kFacings + kFacings) % kFacings), true);
    }
}

} // namespace app
