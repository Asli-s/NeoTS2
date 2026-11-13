// LotLife's part in Buy mode: the catalog, and making, holding, putting back and selling objects.
#include "lot_life.hpp"

#include "diag/log.hpp"
#include "sim/snap.hpp"
#include "sim/vm.hpp"
#include "sim/vm_execution.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>

namespace app {

namespace {

constexpr std::size_t kDirectionData = 1, kAllowedHeightsData = 4;
constexpr std::int16_t kOnTheFloor = 0x01;

} // namespace

const std::vector<ts2::sim::catalog::Item>& LotLife::buyItems() {
    if (!buyItems_) {
        buyItems_ = ts2::sim::catalog::buyItems(sim_->code());
        ts2::diag::info(std::format("buy mode: {} objects in the catalog", buyItems_->size()));
    }
    return *buyItems_;
}

std::string LotLife::catalogText(const ts2::sim::catalog::Item& item, std::size_t line) {
    return sim_->code().catalogString(item.group, item.catalogStrings, line).value_or(std::string());
}

std::int16_t LotLife::makeForBuying(std::uint32_t guid) {
    try {
        ts2::sim::SimObject* made = sim_->vm().create(guid);
        if (made == nullptr) {
            return 0;
        }
        ts2::diag::step(std::format("buy mode: made {} {}", made->definition.name, made->id));
        return made->id;
    } catch (const std::exception& e) {
        ts2::diag::warning(std::format("buy mode: object {:08x} can't be made: {}", guid, e.what()));
        return 0;
    }
}

bool LotLife::standsOnFloor(std::int16_t object) {
    const auto* found = sim_->world().find(object);
    return found != nullptr && (found->data[kAllowedHeightsData] & kOnTheFloor) != 0;
}

LotLife::BuySpot LotLife::buySpot(const std::array<float, 3>& scenePoint, int storey) const {
    const auto [x, y] = routePoint(scenePoint);
    // The middle of the tile under the point.
    const auto middle = [](float v) { return static_cast<std::int32_t>((std::floor(v) + 0.5f) * 65536.0f); };
    return {middle(x), middle(y), storey};
}

bool LotLife::holdAt(std::int16_t object, const BuySpot& spot, std::int16_t direction) {
    auto& world = sim_->world();
    ts2::sim::SimObject* found = world.find(object);
    if (found == nullptr) {
        return false;
    }
    ts2::sim::SnapTarget target;
    target.x = spot.x;
    target.y = spot.y;
    target.level = spot.level;
    target.facing = direction;
    target.checkRoom = false;
    target.checkWalls = false;
    ts2::sim::snapTo(world, *found, target);
    const auto& fits = world.objectFits();
    return !fits || fits(*found, static_cast<float>(spot.x) / 65536.0f, static_cast<float>(spot.y) / 65536.0f, spot.level, direction);
}

std::optional<LotLife::Placed> LotLife::placed(std::int16_t object) {
    const auto* found = sim_->world().find(object);
    if (found == nullptr || found->x == ts2::sim::kOffLot || found->y == ts2::sim::kOffLot) {
        return std::nullopt;
    }
    return Placed{{found->x, found->y, found->level}, found->data[kDirectionData]};
}

void LotLife::putBack(std::int16_t object, const Placed& where) {
    holdAt(object, where.spot, where.direction);
}

bool LotLife::movable(std::int16_t object) {
    const auto* found = sim_->world().find(object);
    if (found == nullptr || found->removed || found->isPerson() || found->x == ts2::sim::kOffLot || found->y == ts2::sim::kOffLot) {
        return false;
    }
    if (found->data[2] != 0) {  // in another object's slot (on a table, in a hand)
        return false;
    }
    const std::uint32_t guid = found->definition.guid();
    if (std::find(std::begin(ts2::sim::vm_detail::kProtectedGuids), std::end(ts2::sim::vm_detail::kProtectedGuids), guid) != std::end(ts2::sim::vm_detail::kProtectedGuids)) {
        return false;
    }
    const auto& d = found->definition;
    return ts2::sim::catalog::isBuyItem(d) && d.field(ts2::objd::field::kMultiTileMasterId) == 0;
}

std::int32_t LotLife::currentValue(std::int16_t object) {
    const auto* found = sim_->world().find(object);
    return found != nullptr ? std::max<std::int32_t>(0, found->data[ts2::sim::catalog::kCurrentValueData]) : 0;
}

void LotLife::setCurrentValue(std::int16_t object, std::int32_t value) {
    if (auto* found = sim_->world().find(object)) {
        found->data[ts2::sim::catalog::kCurrentValueData] = static_cast<std::int16_t>(std::clamp<std::int32_t>(value, 0, 32767));
    }
}

std::uint32_t LotLife::typeOf(std::int16_t object) {
    const auto* found = sim_->world().find(object);
    return found != nullptr ? found->definition.guid() : 0;
}

void LotLife::removeObject(std::int16_t object) {
    auto& world = sim_->world();
    ts2::sim::SimObject* found = world.find(object);
    if (found == nullptr) {
        return;
    }
    constexpr std::size_t kRoomData = 0x1D;
    ts2::sim::takeOutOfSlot(world, *found);
    // Off the lot, as Create Object's "out of world" leaves an object: no tile and room -1.
    found->x = found->y = ts2::sim::kOffLot;
    found->tile.reset();
    found->data[kRoomData] = -1;
    world.markFootprintChanged(found->id);
    world.remove(*found);
    try {
        sim_->vm().tellSimsObjectGoes(object);
    } catch (const std::exception& e) {
        ts2::diag::warning(std::format("buy mode: the Sims couldn't forget object {}: {}", object, e.what()));
    }
}

void LotLife::addFunds(std::int32_t amount) {
    auto globals = sim_->globals();
    ts2::sim::setFunds(globals, static_cast<std::int64_t>(ts2::sim::funds(globals)) + amount);
}

} // namespace app
