// What Buy mode's hand asks of the lot: which objects it can lift, whether one fits a spot, and
// moving it there.
#include "lot_life.hpp"

#include "formats/objd.hpp"
#include "sim/recolour_rows.hpp"
#include "sim/snap.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace app {

namespace {

constexpr std::size_t kDirectionField = 1, kContainerField = 2;

} // namespace

namespace {

// Whether `o` is in use: its use count (object data 0x3E) is above 0, or bit 0x20 of its data 8 is set.
bool inUse(const ts2::sim::SimObject& o) {
    constexpr std::size_t kFlagsField = 8, kUseCountField = 0x3E;
    constexpr std::int16_t kInUseFlag = 0x20;
    return o.data[kUseCountField] > 0 || (o.data[kFlagsField] & kInUseFlag) != 0;
}

// What sits in `container`'s slots, and in theirs.
void slotContents(ts2::sim::World& world, const ts2::sim::SimObject& container, std::vector<std::int16_t>& out, int depth = 0) {
    constexpr int kDepthLimit = 8;  // slots inside slots, and a broken save can make a loop
    if (depth >= kDepthLimit) {
        return;
    }
    for (const std::int16_t id : container.slots) {
        const auto* inside = id > 0 ? world.find(id) : nullptr;
        if (inside != nullptr && inside->data[kContainerField] == container.id) {
            out.push_back(id);
            slotContents(world, *inside, out, depth + 1);
        }
    }
}

} // namespace

// The game lets the hand lift an object only while bit 0x02 of its data 0x2B is set, then while no
// part of it is in use, no Sim sits in its slots (unless bit 0x80 of its data 0x2B is set) and
// nothing in its slots is in use, and last while bit 0x20 of its data 0x2B is clear. Not done yet:
// one more rule for some lot types.
std::optional<LotLife::HandRefusal> LotLife::handRefusal(std::uint32_t object) {
    constexpr std::size_t kMoveFlagsField = 0x2B;
    constexpr std::int16_t kMovable = 0x02, kNotMovable = 0x20, kSimsInSlotsAllowed = 0x80;
    constexpr int kCantMove = 0x26, kInUse = 0x27, kSimInSlots = 0x39, kSlotsInUse = 0x3A;
    const std::uint32_t lead = handLead(object);
    const auto* o = lotObject(lead);
    if (o == nullptr || o->removed || o->isPerson() || o->x == ts2::sim::kOffLot || o->data[kContainerField] > 0) {
        return HandRefusal{};
    }
    if (o->definition.isMultiTileMaster()) {
        return HandRefusal{};
    }
    const std::int16_t move = o->data[kMoveFlagsField];
    if ((move & kMovable) == 0) {
        return HandRefusal{kCantMove, lead};
    }
    auto& world = sim_->world();
    const auto group = handGroup(o->id);
    for (const std::int16_t id : group) {
        const auto* part = world.find(id);
        if (part != nullptr && inUse(*part)) {
            return HandRefusal{kInUse, lead};
        }
    }
    // Sims anywhere in the slots, slots inside slots too, but only what sits directly in a slot for
    // the in-use test. Both point at the object the hand would hold.
    if ((move & kSimsInSlotsAllowed) == 0 && simInSlots(object)) {
        return HandRefusal{kSimInSlots, lead};
    }
    for (const std::int16_t id : group) {
        const auto* part = world.find(id);
        if (part == nullptr) {
            continue;
        }
        for (const std::int16_t c : part->slots) {
            const auto* inside = c > 0 ? world.find(c) : nullptr;
            if (inside != nullptr && inside->data[kContainerField] == part->id && inUse(*inside)) {
                return HandRefusal{kSlotsInUse, lead};
            }
        }
    }
    if ((move & kNotMovable) != 0) {
        return HandRefusal{kCantMove, lead};
    }
    return std::nullopt;
}

bool LotLife::simInSlots(std::uint32_t object) {
    auto& world = sim_->world();
    for (const std::int16_t id : handGroup(handLead(object))) {
        const auto* part = world.find(id);
        if (part == nullptr) {
            continue;
        }
        std::vector<std::int16_t> contents;
        slotContents(world, *part, contents);
        for (const std::int16_t c : contents) {
            const auto* inside = world.find(c);
            if (inside != nullptr && inside->isPerson()) {
                return true;
            }
        }
    }
    return false;
}

std::uint32_t LotLife::handLead(std::uint32_t object) {
    const auto* o = lotObject(object);
    if (o == nullptr || o->definition.field(ts2::objd::field::kMultiTileMasterId) == 0 || o->definition.isMultiTileMaster()) {
        return object;
    }
    const auto* lead = sim_->world().find(o->data[ts2::sim::kLeadTileField]);
    return lead != nullptr && !lead->removed ? static_cast<std::uint32_t>(lead->id) : object;
}

std::vector<std::int16_t> LotLife::handGroup(std::uint32_t object) {
    const auto* o = lotObject(object);
    if (o == nullptr) {
        return {};
    }
    const std::int16_t lead = o->data[ts2::sim::kLeadTileField];
    if (o->definition.field(ts2::objd::field::kMultiTileMasterId) == 0 || o->definition.isMultiTileMaster() || lead <= 0) {
        return {o->id};
    }
    std::vector<std::int16_t> group{o->id};
    for (const std::int16_t id : sim_->world().idsIncludingRemoved([&](const ts2::sim::SimObject& p) {
             return p.id != o->id && p.data[ts2::sim::kLeadTileField] == lead && p.definition.field(ts2::objd::field::kMultiTileMasterId) != 0;
         })) {
        group.push_back(id);
    }
    return group;
}

std::optional<std::array<float, 3>> LotLife::handScenePoint(std::uint32_t object, const ts2::scene::Scene& scene) const {
    for (const auto& p : scene.placedObjects) {
        if (p.object == object && p.instance < scene.instances.size()) {
            // Where the object stands, not where its clip slides it.
            const auto base = clipBases_.find(p.instance);
            const auto& m = base != clipBases_.end() ? base->second.m : scene.instances[p.instance].transform.m;
            return std::array<float, 3>{m[3], m[7], m[11]};
        }
    }
    return std::nullopt;
}

void LotLife::setHandRefused(std::uint32_t object) {
    handRefused_ = object;
    handRefusedParts_.clear();
    if (object != 0) {
        for (const std::int16_t id : handGroup(object)) {
            handRefusedParts_.insert(static_cast<std::uint32_t>(id));
        }
    }
}

std::optional<LotLife::HandSpot> LotLife::handSpotOf(std::uint32_t object) {
    const auto* o = lotObject(object);
    if (o == nullptr || o->x == ts2::sim::kOffLot) {
        return std::nullopt;
    }
    return HandSpot{static_cast<float>(o->x) / 65536.0f, static_cast<float>(o->y) / 65536.0f, o->level, o->data[kDirectionField]};
}

// The game snaps the hand to the middle of the tile under the cursor.
LotLife::HandSpot LotLife::handSpotAt(const std::array<float, 3>& scenePoint, int storey, std::int16_t facing, bool snap) const {
    const auto [x, y] = routePoint(scenePoint);
    if (!snap) {
        return {x, y, storey, facing};
    }
    return {std::floor(x) + 0.5f, std::floor(y) + 0.5f, storey, facing};
}

std::optional<ts2::sim::LotSimulation::PlaceRefusal> LotLife::handPlaceRefusal(std::uint32_t object, const HandSpot& spot) {
    const auto* o = lotObject(object);
    if (o == nullptr) {
        return ts2::sim::LotSimulation::PlaceRefusal{0, spot.x, spot.y, spot.storey};
    }
    return sim_->groupRefusal(*o, spot.x, spot.y, spot.storey, spot.facing);
}

std::uint32_t LotLife::handPutDownSound(std::uint32_t object) {
    constexpr std::uint32_t kPutDown = 0xFF600621, kPutDownMarked = 0xFFFF2F08;  // sound ids, leakcheck: ok
    constexpr std::size_t kSoundFlagsField = 0x45;
    constexpr std::uint16_t kMarked = 0x04;
    const auto* o = lotObject(object);
    const auto* type = o != nullptr ? sim_->code().objectType(o->definition.guid()) : nullptr;
    const auto* master = type != nullptr ? sim_->code().multiTileMaster(*type) : nullptr;
    return master != nullptr && (master->definition.field(kSoundFlagsField) & kMarked) != 0 ? kPutDownMarked : kPutDown;
}

std::array<std::uint16_t, 2> LotLife::handPlacementFlags(std::uint32_t object) {
    constexpr std::size_t kHeightsField = 4, kPlacementFlagsField = 0x2A;
    const auto* o = lotObject(object);
    return o != nullptr ? std::array<std::uint16_t, 2>{static_cast<std::uint16_t>(o->data[kHeightsField]),
                                                       static_cast<std::uint16_t>(o->data[kPlacementFlagsField])}
                        : std::array<std::uint16_t, 2>{};
}

std::array<float, 3> LotLife::scenePointAt(float x, float y, int storey, const ts2::scene::Scene& scene) const {
    // The lot's x and y are the position's y and x. Out of the lot's frame by its turn and move.
    const float lot[3] = {y, x, scene.floorHeight ? scene.floorHeight(storey, y, x) : 0.0f};
    const auto& m = lotFrame_.m;
    return {m[0] * lot[0] + m[1] * lot[1] + m[2] * lot[2] + m[3], m[4] * lot[0] + m[5] * lot[1] + m[6] * lot[2] + m[7],
            m[8] * lot[0] + m[9] * lot[1] + m[10] * lot[2] + m[11]};
}

void LotLife::handMove(std::uint32_t object, const HandSpot& spot) {
    auto* o = lotObject(object);
    if (o == nullptr) {
        return;
    }
    constexpr float kFixed = 65536.0f;
    ts2::sim::snapTo(sim_->world(), *o,
                     ts2::sim::SnapTarget{static_cast<std::int32_t>(std::lround(spot.x * kFixed)), static_cast<std::int32_t>(std::lround(spot.y * kFixed)),
                                          spot.storey, spot.facing, false, false});
}

// An object sells for its value: object data 0x29, or data 0x69 while bit 0x01 of data 0x6A is set,
// or nothing while bit 0x02 is. A broken object (data 0x0F) sells for half. Rewards (OBJD field 0x28
// bit 0x400 aspiration or 0x800 career) sell for nothing. A multi-tile object sells for its lead's value,
// halved when any of its parts is broken.
std::optional<LotLife::HandSale> LotLife::handSale(std::uint32_t object) {
    constexpr std::size_t kValueField = 0x29, kOtherValueField = 0x69, kValueFlagsField = 0x6A, kRepairField = 0x0F;
    constexpr std::size_t kFunctionSortField = 0x28;
    constexpr std::uint16_t kRewardBits = 0x0C00;
    const auto* o = lotObject(object);
    if (o == nullptr) {
        return std::nullopt;
    }
    const auto flags = static_cast<std::uint16_t>(o->data[kValueFlagsField]);
    std::int32_t value = (flags & 0x02) != 0 ? 0 : (flags & 0x01) != 0 ? o->data[kOtherValueField] : o->data[kValueField];
    const auto group = handGroup(object);
    if (std::any_of(group.begin(), group.end(), [&](std::int16_t id) {
            const auto* part = sim_->world().find(id);
            return part != nullptr && part->data[kRepairField] != 0;
        })) {
        value /= 2;
    }
    if ((o->definition.field(kFunctionSortField) & kRewardBits) != 0) {
        value = 0;
    }
    // A multi-tile object's name and picture are its master's, the catalog item.
    const auto* type = sim_->code().objectType(o->definition.guid());
    const auto* master = type != nullptr && o->definition.field(ts2::objd::field::kMultiTileMasterId) != 0 ? sim_->code().multiTileMaster(*type) : nullptr;
    return HandSale{(master != nullptr ? master->definition : o->definition).guid(), value};
}

// An object can be deleted while bit 0x02 of its data 0x6A is set, or while bit 0x40 of its data
// 0x2B is clear. Not done yet: the object function the game asks first when an object has one.
std::optional<std::int32_t> LotLife::handSell(std::uint32_t object) {
    constexpr std::size_t kValueFlagsField = 0x6A, kKeepField = 0x2B;
    auto* o = lotObject(object);
    const auto sale = handSale(object);
    if (o == nullptr || !sale) {
        return std::nullopt;
    }
    const bool deletable = (o->data[kValueFlagsField] & 0x02) != 0 || (o->data[kKeepField] & 0x40) == 0;
    if (!deletable) {
        return std::nullopt;
    }
    handKeep(object, sale->value);
    return sale->value;
}

// A sold object goes with its other parts and what sits in its slots, as the game's delete takes
// them. Only the object's own value is paid for.
bool LotLife::handKeep(std::uint32_t object, std::int32_t fundsChange) {
    auto& world = sim_->world();
    auto* o = lotObject(object);
    if (o == nullptr) {
        return false;
    }
    std::vector<std::int16_t> kept;
    for (const std::int16_t id : handGroup(object)) {
        if (const auto* part = world.find(id)) {
            kept.push_back(id);
            slotContents(world, *part, kept);
        }
    }
    for (const std::int16_t id : kept) {
        if (auto* k = world.find(id)) {
            world.keepSold(*k);
            handSold_.insert(id);
        }
    }
    handKeptWith_[o->id] = std::move(kept);
    ts2::sim::setFunds(sim_->globals(), std::int64_t{funds()} + fundsChange);
    return true;
}

bool LotLife::handRestore(std::uint32_t object, std::int32_t fundsChange) {
    auto& world = sim_->world();
    const auto id = static_cast<std::int16_t>(object);
    const auto kept = handKeptWith_.find(id);
    if (world.findIncludingRemoved(id) == nullptr || kept == handKeptWith_.end()) {
        return false;
    }
    for (const std::int16_t k : kept->second) {
        if (auto* back = world.findIncludingRemoved(k); back != nullptr && handSold_.contains(k)) {
            world.restoreKept(*back);
            handSold_.erase(k);
        }
    }
    handKeptWith_.erase(kept);
    ts2::sim::setFunds(sim_->globals(), std::int64_t{funds()} + fundsChange);
    return true;
}

std::optional<std::uint32_t> LotLife::handCreate(std::uint32_t guid) {
    const auto* type = sim_->code().objectType(guid);
    if (type == nullptr || (!type->definition.isMultiTileMaster() && type->definition.field(ts2::objd::field::kMultiTileMasterId) != 0)) {
        return std::nullopt;
    }
    try {
        auto* made = type->definition.isMultiTileMaster() ? sim_->vm().createMultiTile(guid) : sim_->vm().create(guid);
        if (made != nullptr) {
            return static_cast<std::uint32_t>(made->id);
        }
    } catch (const std::exception&) {
        // Out of object ids, or a start the simulator stopped: nothing to hold.
    }
    return std::nullopt;
}

std::optional<std::pair<ts2::sim::ChosenRecolour, std::uint32_t>> LotLife::handRecolourOf(std::uint32_t object) {
    for (const std::int16_t id : handGroup(object)) {
        if (const auto* part = sim_->world().find(id); part != nullptr && !part->recolours.empty()) {
            return std::pair{part->recolours.front(), part->group};
        }
    }
    return std::nullopt;
}

void LotLife::handRecolour(std::uint32_t object, std::span<const ts2::content::RecolourChoice> choices) {
    constexpr std::uint16_t kModelNames = 0x85;
    constexpr std::size_t kModelIndexLow = 0x48, kModelIndexHigh = 0x49;
    const auto bare = [](std::string m) {
        std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (m.ends_with("_cres")) {
            m.resize(m.size() - 5);
        }
        return m;
    };
    auto& code = sim_->code();
    const auto sale = handSale(object);
    const std::uint32_t guid = sale ? sale->guid : 0;
    for (const std::int16_t id : handGroup(object)) {
        auto* part = sim_->world().find(id);
        if (part == nullptr) {
            continue;
        }
        const auto& d = part->definition;
        const std::uint32_t index = d.field(kModelIndexLow) | static_cast<std::uint32_t>(d.field(kModelIndexHigh)) << 16;
        const auto model = code.string(part->group, kModelNames, index);
        // A subset's recolours may be filed under another model (its tsMaterialsMeshName), or, when
        // its node names none, be the materials made for the catalog item.
        const auto takes = [&](const ts2::content::RecolourChoice& choice) {
            if (!model) {
                return false;
            }
            if (bare(*model) == bare(choice.model)) {
                return true;
            }
            const std::string one[] = {*model};
            const auto subsets = designSubsetsOf(one, guid);
            return std::any_of(subsets.begin(), subsets.end(), [&](const auto& d) {
                if (bare(d.subset) != bare(choice.subset)) {
                    return false;
                }
                if (!d.mesh.empty()) {
                    return bare(d.mesh) == bare(choice.model);
                }
                const auto made = sim_->recolours().materialsOf(guid);
                return std::any_of(made.begin(), made.end(), [&](const auto* m) {
                    return bare(m->model) == bare(choice.model) && bare(m->subset) == bare(choice.subset) && bare(m->name) == bare(choice.material);
                });
            });
        };
        for (const auto& choice : choices) {
            if (!takes(choice) ||
                !ts2::sim::applyRecolour(code, *part, sim_->recolours(), choice.model, choice.subset, choice.material)) {
                continue;
            }
            std::erase_if(part->recolours, [&](const ts2::sim::ChosenRecolour& had) { return bare(had.model) == bare(choice.model) && had.subset == choice.subset; });
            part->recolours.push_back({choice.model, choice.subset, choice.material});
            std::erase_if(part->materialChanges, [&](const ts2::sim::MaterialChange& c) { return c.group == choice.subset; });
            part->materialChanges.push_back({choice.subset, choice.material});
            ++part->materialVersion;
        }
    }
}

std::vector<std::pair<std::string, std::uint32_t>> LotLife::designModels(std::uint32_t object) {
    constexpr std::uint16_t kModelNames = 0x85;
    constexpr std::size_t kModelIndexLow = 0x48, kModelIndexHigh = 0x49;
    std::vector<std::pair<std::string, std::uint32_t>> out;
    auto& code = sim_->code();
    for (const std::int16_t id : handGroup(object)) {
        const auto* part = sim_->world().find(id);
        if (part == nullptr) {
            continue;
        }
        const auto& d = part->definition;
        const std::uint32_t index = d.field(kModelIndexLow) | static_cast<std::uint32_t>(d.field(kModelIndexHigh)) << 16;
        if (auto model = code.string(part->group, kModelNames, index); model && !model->empty()) {
            out.emplace_back(std::move(*model), part->group);
        }
    }
    return out;
}

std::vector<ts2::content::DesignSubset> LotLife::designSubsetsOf(std::span<const std::string> models, std::uint32_t guid) {
    constexpr std::size_t kMostSubsets = 2;
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    std::vector<ts2::content::DesignSubset> out;
    for (const auto& model : models) {
        std::string key = lower(model);
        if (key.ends_with("_cres")) {
            key.resize(key.size() - 5);
        }
        auto it = designSubsetsByModel_.find(key);
        if (it == designSubsetsByModel_.end()) {
            it = designSubsetsByModel_.emplace(key, assets_.designSubsets(key)).first;
        }
        for (const auto& d : it->second) {
            if (out.size() < kMostSubsets &&
                std::none_of(out.begin(), out.end(), [&](const auto& had) { return lower(had.subset) == lower(d.subset); })) {
                out.push_back({d.subset, d.mesh, model, guid});
            }
        }
    }
    return out;
}

std::vector<ts2::content::DesignSubset> LotLife::designSubsets(std::uint32_t object) {
    std::vector<std::string> models;
    for (auto& [model, group] : designModels(object)) {
        models.push_back(std::move(model));
    }
    const auto sale = handSale(object);
    return designSubsetsOf(models, sale ? sale->guid : 0);
}

std::vector<ts2::content::RecolourStrip> LotLife::designStrips(std::uint32_t object) {
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const auto& catalog = sim_->recolours();
    auto strips = ts2::content::recolourStrips(catalog, designSubsets(object));
    // Each strip starts at the family of the object's recolour of its subset, else at the default.
    for (auto& strip : strips) {
        for (const std::int16_t id : handGroup(object)) {
            const auto* part = sim_->world().find(id);
            if (part == nullptr) {
                continue;
            }
            for (const auto& had : part->recolours) {
                const auto* material = lower(had.subset) == lower(strip.subset) ? catalog.find(had.model, had.subset, had.material) : nullptr;
                if (material == nullptr) {
                    continue;
                }
                for (std::size_t i = 0; i < strip.choices.size(); ++i) {
                    if (lower(strip.choices[i].family) == lower(material->family)) {
                        strip.chosen = i;
                    }
                }
            }
        }
    }
    return strips;
}

void LotLife::designKeep(std::uint32_t object) {
    designKept_.clear();
    for (const std::int16_t id : handGroup(object)) {
        if (const auto* part = sim_->world().find(id)) {
            designKept_.push_back({id, part->materialChanges, part->recolours, part->materialNames, part->savedMaterialNames});
        }
    }
}

void LotLife::designRestore() {
    for (auto& kept : designKept_) {
        if (auto* part = sim_->world().find(kept.id)) {
            part->materialChanges = kept.changes;
            part->recolours = kept.recolours;
            part->materialNames = kept.names;
            part->savedMaterialNames = kept.savedNames;
            ++part->materialVersion;
        }
    }
}

// The game asks only whether the object has a subset to recolour, not whether it has recolours.
bool LotLife::designable(std::uint32_t object) {
    if (lotObject(object) == nullptr || isSim(object)) {
        return false;
    }
    return !designSubsets(object).empty();
}

bool LotLife::handTurnsToWalls(std::uint32_t object) {
    constexpr std::size_t kWallPlacementField = 0x0D, kCategoryField = 0x3B;
    constexpr std::int16_t kAgainstWalls = 0x5F, kNotTurned = 0x7D;
    const auto* o = lotObject(object);
    return o != nullptr && (o->data[kWallPlacementField] & kAgainstWalls) != 0 && o->data[kCategoryField] != kNotTurned;
}

// The eyedropper and the sledgehammer take an object while bit 0x02 of its data 0x2B is set and bit
// 0x20 is clear (the sledgehammer takes category 0xA7 either way), and never one of category 0xB6.
// The eyedropper also needs it not a reward (OBJD 0x28 bits 0x0C00) and, unless its OBJD field 206 is
// 0 (diagonal doors and windows), in its catalog (OBJD 0x45 above 0, or 0x28 and 0x5E above 0, or 0x28
// set, 0x5E clear and 0x64 above 0). Not done yet: objects handed to other tools, locked tiles and the
// moveObjects cheat.
bool LotLife::toolTakes(std::uint32_t object, bool sledgehammer) {
    constexpr std::size_t kMoveFlagsField = 0x2B, kCategoryField = 0x3B;
    constexpr std::int16_t kMovable = 0x02, kNotMovable = 0x20;
    constexpr std::int16_t kAlwaysSmashed = 0xA7, kNeverTaken = 0xB6, kOtherTools = 0x76;
    constexpr int kOtherToolTypes[] = {11, 12};
    constexpr std::size_t kBuildType = 0x45, kFunctionSort = 0x28, kSubsort = 0x5E, kOtherSort = 0x64;
    constexpr std::uint16_t kRewardBits = 0x0C00;
    const auto* o = lotObject(object);
    if (o == nullptr || o->removed || o->isPerson() || o->x == ts2::sim::kOffLot) {
        return false;
    }
    const std::int16_t category = o->data[kCategoryField], move = o->data[kMoveFlagsField];
    const int type = o->definition.field(ts2::objd::field::kType);
    if (category == kOtherTools || std::find(std::begin(kOtherToolTypes), std::end(kOtherToolTypes), type) != std::end(kOtherToolTypes)) {
        return false;
    }
    if (category == kNeverTaken) {
        return false;
    }
    if (!(sledgehammer && category == kAlwaysSmashed) && ((move & kMovable) == 0 || (move & kNotMovable) != 0)) {
        return false;
    }
    if (sledgehammer) {
        return true;
    }
    if ((o->definition.field(kFunctionSort) & kRewardBits) != 0) {
        return false;
    }
    const auto sale = handSale(object);
    const auto* item = sale ? sim_->code().objectType(sale->guid) : nullptr;
    if (item == nullptr) {
        return false;
    }
    constexpr std::size_t kCatalogTestField = 206;
    if (item->definition.scriptField(kCatalogTestField).value_or(0) == 0) {
        return true;
    }
    const auto field = [&](std::size_t i) { return static_cast<std::int16_t>(item->definition.field(i)); };
    return field(kBuildType) > 0 || (field(kFunctionSort) > 0 && field(kSubsort) > 0) ||
           (field(kFunctionSort) != 0 && field(kSubsort) == 0 && field(kOtherSort) > 0);
}

// The eyedropper's copy is the item named by OBJD field 207 when it has one (a diagonal door's straight
// version), else the object's own catalog item.
std::optional<std::uint32_t> LotLife::eyedropGuid(std::uint32_t object) {
    constexpr std::size_t kStraightVersionField = 207;
    const auto sale = handSale(object);
    if (!sale) {
        return std::nullopt;
    }
    const auto* item = sim_->code().objectType(sale->guid);
    const auto other = item != nullptr ? static_cast<std::uint32_t>(item->definition.scriptField(kStraightVersionField).value_or(0)) : 0;
    return other != 0 && sim_->code().objectType(other) != nullptr ? other : sale->guid;
}

// An item whose OBJD field 206 is 0 is a diagonal version and names its straight one in field 207.
// Others name their diagonal version, if any, in field 206.
std::optional<std::uint32_t> LotLife::otherVersion(std::uint32_t guid) {
    constexpr std::size_t kDiagonalVersionField = 206, kStraightVersionField = 207;
    const auto* item = sim_->code().objectType(guid);
    if (item == nullptr) {
        return std::nullopt;
    }
    const auto diagonal = static_cast<std::uint32_t>(item->definition.scriptField(kDiagonalVersionField).value_or(0));
    const auto other = diagonal == 0 ? static_cast<std::uint32_t>(item->definition.scriptField(kStraightVersionField).value_or(0)) : diagonal;
    if (other == 0 || other == guid || sim_->code().objectType(other) == nullptr) {
        return std::nullopt;
    }
    return other;
}

bool LotLife::isDoorOrWindow(std::uint32_t guid) {
    constexpr int kDoorType = 8, kWindowType = 9;
    const auto* item = sim_->code().objectType(guid);
    const int type = item != nullptr ? item->definition.field(ts2::objd::field::kType) : 0;
    return type == kDoorType || type == kWindowType;
}

std::vector<ts2::content::RecolourChoice> LotLife::eyedropColours(std::uint32_t object) {
    const auto catalog = ts2::content::recolourStrips(sim_->recolours(), designSubsets(object));
    const auto had = designStrips(object);
    std::vector<ts2::content::RecolourChoice> out;
    for (std::size_t k = 0; k < had.size() && k < catalog.size(); ++k) {
        if (had[k].chosen != catalog[k].chosen && had[k].chosen < had[k].choices.size()) {
            out.push_back(had[k].choices[had[k].chosen]);
        }
    }
    return out;
}

void LotLife::handDrop(std::uint32_t object) {
    for (const std::int16_t id : handGroup(object)) {
        if (auto* o = sim_->world().find(id)) {
            sim_->world().remove(*o);
        }
    }
}

std::int32_t LotLife::handPrice(std::uint32_t object) {
    const auto* o = lotObject(object);
    if (o == nullptr) {
        return 0;
    }
    const auto* type = sim_->code().objectType(o->definition.guid());
    const bool part = o->definition.field(ts2::objd::field::kMultiTileMasterId) != 0;
    const auto* master = type != nullptr && part ? sim_->code().multiTileMaster(*type) : nullptr;
    return (master != nullptr ? master->definition : o->definition).field(ts2::objd::field::kPrice);
}

std::int16_t LotLife::handFacing(std::uint32_t object) {
    const auto* o = lotObject(object);
    return o != nullptr ? o->data[kDirectionField] : 0;
}

bool LotLife::handAffords(std::uint32_t guid) {
    const auto* type = sim_->code().objectType(guid);
    const std::int32_t price = type != nullptr ? type->definition.field(ts2::objd::field::kPrice) : 0;
    return price <= 0 || price <= funds();
}

// The game moves and copies a fireplace with a tool of its own, picked when the object's
// "CT - Get Link Data" returns true with Temp 2, the piece's type, at 1 or 4 (a mantle or a hearth).
bool LotLife::handIsFireplace(std::uint32_t object) {
    constexpr const char* kLinkData = "CT - Get Link Data";
    constexpr std::size_t kPieceTemp = 2;
    constexpr std::int16_t kMantle = 1, kHearth = 4;
    auto& code = sim_->code();
    for (const std::int16_t id : handGroup(object)) {
        auto* part = sim_->world().find(id);
        if (part == nullptr || part->removed) {
            continue;
        }
        for (const std::uint32_t group : {part->group, code.semiGlobalGroup(part->group), ts2::sim::kGlobalGroup}) {
            const auto tree = group != 0 ? code.behaviorByName(group, kLinkData) : std::nullopt;
            if (!tree) {
                continue;
            }
            const auto r = sim_->vm().runOnObjectNow(*part, part->group, *tree, 0, {});
            const std::int16_t piece = part->thread.temps[kPieceTemp];
            if (r.outcome == ts2::sim::Outcome::True && (piece == kMantle || piece == kHearth)) {
                return true;
            }
            break;
        }
    }
    return false;
}

// An item with OBJD field 0x60 set gives another copy, and so does any item when `always` is set.
// Never a fireplace, whose tool never gives one, one with OBJD 0x28 bits 0x0C00, one whose lead part
// has OBJD field 0x2E set, or straight stairs (object type 10). Whether the game's hand repeats
// stairs is not known yet.
bool LotLife::handRepeats(std::uint32_t object, std::uint32_t guid, bool always) {
    constexpr std::size_t kRepeatField = 0x60, kFunctionSortField = 0x28, kNoRepeatField = 0x2E;
    constexpr std::uint16_t kNoRepeatBits = 0x0C00;
    constexpr int kStairsType = 10;
    const auto* type = sim_->code().objectType(guid);
    const auto* lead = lotObject(handLead(object));
    if (type == nullptr || lead == nullptr || type->definition.field(ts2::objd::field::kType) == kStairsType ||
        (type->definition.field(kFunctionSortField) & kNoRepeatBits) != 0 || lead->definition.field(kNoRepeatField) != 0 ||
        handIsFireplace(object)) {
        return false;
    }
    return always || type->definition.field(kRepeatField) != 0;
}

void LotLife::handFinishSales() {
    auto& world = sim_->world();
    for (const std::int16_t id : handSold_) {
        if (auto* o = world.findIncludingRemoved(id)) {
            world.remove(*o);
        }
    }
    handSold_.clear();
    handKeptWith_.clear();
}

// A marker stands where each part of the held object does, turned as it is, and markers are drawn
// only while the hand holds something. A clip sliding the object doesn't move them. Often one part
// draws the whole object, so a part without a model of its own gets its marker at its tile's offset
// from the drawn part, at the drawn part's height.
void LotLife::placeHandMarker(ts2::scene::Scene& scene) {
    static constexpr const char* kModels[2] = {"objectPlacementIconGood", "objectPlacementIconBad"};
    struct Held {
        ts2::scene::Transform transform;
        int level = 0;
        int lightSet = -1;
    };
    std::vector<Held> held;
    const auto drawn = [&](std::int16_t part) -> std::optional<Held> {
        for (const auto& p : scene.placedObjects) {
            if (p.object == static_cast<std::uint32_t>(part) && p.instance < scene.instances.size()) {
                const auto& i = scene.instances[p.instance];
                // Where the part stands, not where its clip slides it (a pushed in chair).
                const auto base = clipBases_.find(p.instance);
                return Held{base != clipBases_.end() ? base->second : i.transform, i.level, i.lightSet};
            }
        }
        return std::nullopt;
    };
    const auto group = handMarked_ != 0 ? handGroup(handMarked_) : std::vector<std::int16_t>{};
    const ts2::sim::SimObject* reference = nullptr;
    std::optional<Held> referenceDrawn;
    for (const std::int16_t part : group) {
        if ((referenceDrawn = drawn(part))) {
            reference = sim_->world().find(part);
            break;
        }
    }
    for (const std::int16_t part : group) {
        const auto* o = sim_->world().find(part);
        if (reference == nullptr || o == nullptr || o->x == ts2::sim::kOffLot) {
            continue;
        }
        if (auto own = drawn(part)) {
            held.push_back(*own);
            continue;
        }
        Held at = *referenceDrawn;
        const auto d = sceneDirection(static_cast<float>(o->x - reference->x) / 65536.0f, static_cast<float>(o->y - reference->y) / 65536.0f);
        at.transform.m[3] += d[0];
        at.transform.m[7] += d[1];
        at.transform.m[11] += d[2];
        held.push_back(at);
    }
    for (std::size_t k = 0; k < 2; ++k) {
        const std::size_t shown = (k == 0) == handMarkerGood_ ? held.size() : 0;
        while (handMarkers_[k].size() < shown) {
            ts2::scene::Instance marker;
            marker.mesh = objectMaterials_.model(kModels[k], {}, scene);
            marker.name = kModels[k];
            marker.part = ts2::scene::Part::Object;
            marker.transform.m.fill(0.0f);
            handMarkers_[k].push_back(scene.instances.size());
            scene.instances.push_back(std::move(marker));
        }
        for (std::size_t n = 0; n < handMarkers_[k].size(); ++n) {
            auto& instance = scene.instances[handMarkers_[k][n]];
            ts2::scene::Transform place;
            place.m.fill(0.0f);
            if (n < shown) {
                place = held[n].transform;
            }
            if (instance.transform.m != place.m) {
                instance.transform = place;
                if (n < shown) {
                    instance.level = held[n].level;
                    instance.lightSet = held[n].lightSet;
                }
                changedInstances_.push_back(handMarkers_[k][n]);
            }
        }
    }
}

} // namespace app
