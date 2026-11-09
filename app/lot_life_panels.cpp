// What the live-mode panels read from the lot: pie menus, the queue, wants, names, relationships,
// inventories, memories, motives and portraits.
#include "lot_life.hpp"

#include "content/neighborhood.hpp"
#include "diag/log.hpp"
#include "formats/image.hpp"
#include "sim/ground_click.hpp"
#include "sim/motives.hpp"
#include "sim/pie_menu.hpp"
#include "sim/want_refill.hpp"

#include <algorithm>
#include <climits>
#include <format>
#include <limits>
#include <set>
#include <string_view>

namespace app {

std::int16_t LotLife::aspiration() {
    constexpr std::size_t kAspiration = 46;
    const auto* sim = sim_->world().find(sim_->globals()[ts2::sim::global::kSelectedPerson]);
    return sim ? sim->personData[kAspiration] : 0;
}

namespace {
constexpr std::size_t kAspirationScore = 0xA0, kAgeDaysLeft = 0x5B, kAgeDuration = 0x9C;  // person data
} // namespace

ts2::sim::SimObject* LotLife::lotObject(std::uint32_t id) {
    return id > 0 && id <= static_cast<std::uint32_t>(INT16_MAX) ? sim_->world().find(static_cast<std::int16_t>(id)) : nullptr;
}

const ts2::sim::Inventory* LotLife::ownInventory(std::int16_t sim) {
    const std::int16_t self = neighborId(sim);
    return self ? sim_->world().inventory(ts2::sim::InventoryKind::Neighbor, self) : nullptr;
}

bool LotLife::hoverable(std::uint32_t object, bool shift) {
    auto* found = lotObject(object);
    auto* actor = sim_->world().find(selected());
    if (found == nullptr || actor == nullptr) {
        return false;
    }
    sim_->world().markShiftClick(*found, shift);
    const bool debug = found->shiftClicked && sim_->world().testingCheats;
    return !ts2::sim::pieMenu(sim_->vm(), sim_->code(), *actor, *found, {.debug = debug, .firstOnly = true}).choices.empty();
}

bool LotLife::isSim(std::uint32_t object) {
    const auto* found = lotObject(object);
    return found != nullptr && found->isPerson();
}

std::uint32_t LotLife::partAt(std::uint32_t object, const std::array<float, 3>& scenePoint, const ts2::scene::Scene& scene) {
    const auto* found = lotObject(object);
    const auto multiTile = [](const ts2::sim::SimObject& o) { return o.definition.field(ts2::objd::field::kMultiTileMasterId) != 0; };
    if (found == nullptr || !multiTile(*found) || !scene.floorHeight) {
        return object;
    }
    const std::int16_t lead = found->data[ts2::sim::kLeadTileField];
    std::uint32_t best = object;
    float nearest = std::numeric_limits<float>::max();
    sim_->world().nextAfter(0, [&](const ts2::sim::SimObject& o) {
        if (o.data[ts2::sim::kLeadTileField] != lead || !multiTile(o) || o.removed || o.x == ts2::sim::kOffLot || o.y == ts2::sim::kOffLot) {
            return false;
        }
        const float x = static_cast<float>(o.y) / 65536.0f;  // the lot's x and y are the position's y and x
        const float y = static_cast<float>(o.x) / 65536.0f;
        float spot[3] = {x, y, scene.floorHeight(o.level, x, y)};
        lotFrame_.apply(spot, true);
        const float dx = spot[0] - scenePoint[0], dy = spot[1] - scenePoint[1], dz = spot[2] - scenePoint[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < nearest) {
            nearest = d;
            best = static_cast<std::uint32_t>(o.id);
        }
        return false;
    });
    return best;
}

std::vector<ts2::sim::PieChoice> LotLife::pieMenu(std::uint32_t object, bool shift) {
    auto* found = lotObject(object);
    auto* actor = sim_->world().find(selected());
    if (found == nullptr || actor == nullptr) {
        return {};
    }
    sim_->world().markShiftClick(*found, shift);
    const bool debug = found->shiftClicked && sim_->world().testingCheats;
    return ts2::sim::pieMenu(sim_->vm(), sim_->code(), *actor, *found, {.debug = debug}).choices;
}

LotLife::GroundMenu LotLife::groundPieMenu(const std::array<float, 3>& scenePoint, int storey) {
    const auto* actor = sim_->world().find(selected());
    if (actor == nullptr) {
        return {};
    }
    const auto [x, y] = routePoint(scenePoint);
    auto* made = ts2::sim::groundDestination(sim_->vm(), sim_->world(), actor, {x, y, storey});
    if (made == nullptr) {
        return {};
    }
    ts2::diag::step(std::format("ground clicked at {:.2f} {:.2f} level {}: destination {}", made->x / 65536.0, made->y / 65536.0, made->level,
                                made->id));
    return {static_cast<std::uint32_t>(made->id), ts2::sim::pieMenu(sim_->vm(), sim_->code(), *sim_->world().find(selected()), *made).choices};
}

void LotLife::choose(std::uint32_t object, const ts2::sim::PieChoice& choice) {
    auto* found = lotObject(object);
    auto* actor = sim_->world().find(selected());
    if (found == nullptr || actor == nullptr) {
        return;
    }
    if (ts2::sim::runsAtOnce(sim_->code(), *found, choice)) {
        ts2::diag::step(std::format("Sim {} does \"{}\" on {} {} at once", actor->id, choice.name, found->definition.name, found->id));
        const auto r = ts2::sim::runAtOnce(sim_->vm(), sim_->code(), sim_->world(), *actor, *found, choice);
        if (r.outcome != ts2::sim::Outcome::True && r.outcome != ts2::sim::Outcome::False) {
            ts2::diag::warning(std::format("simulation: \"{}\" stopped: {} {}", choice.name, r.reason, r.detail));
        }
        return;
    }
    if (const auto queued = ts2::sim::queuedChoice(sim_->code(), sim_->world(), *found, choice)) {
        ts2::diag::step(std::format("Sim {} is told to \"{}\" on {} {}", actor->id, choice.name, found->definition.name, found->id));
        ts2::sim::queueInteraction(*actor, *queued);
    }
}

std::vector<LotLife::QueuedAction> LotLife::queue(std::int16_t sim) {
    // A part of a multi-tile object shows its lead (object data 0x13). With no picture named, an
    // object whose OBJD field 0x37 is 0 shows the picture its object data 0 names.
    constexpr std::size_t kGraphicField = 0x00, kGraphicPictureField = 0x37;
    std::vector<QueuedAction> out;
    const auto* person = sim_->world().find(sim);
    if (person == nullptr) {
        return out;
    }
    auto add = [&](const ts2::sim::QueuedInteraction& q, bool running, std::int16_t object, std::uint32_t guid, std::uint32_t index,
                   const std::string& name) {
        QueuedAction a{q.id, running, q.cancelled, q.nested, 0, index, 0, name};
        if (a.name.empty()) {
            const auto* target = sim_->world().find(q.object);
            a.name = target ? ts2::sim::interactionName(sim_->code(), *target, q.index) : std::string();
        }
        if (guid != 0 && guid != UINT32_MAX) {
            a.guid = guid;
        } else if (const auto* icon = sim_->world().find(object)) {
            if (icon->isPerson()) {
                a.neighbor = sim_->world().neighborIdOf(*icon);
            } else {
                const auto* lead = icon->data[ts2::sim::kLeadTileField] != 0 ? sim_->world().find(icon->data[ts2::sim::kLeadTileField]) : nullptr;
                const auto& shown = lead ? *lead : *icon;
                a.guid = shown.definition.guid();
                if (a.index == 0 && shown.definition.field(kGraphicPictureField) == 0) {
                    a.index = static_cast<std::uint16_t>(shown.data[kGraphicField]);
                }
            }
        } else {
            return;  // no icon: the queue leaves it out
        }
        out.push_back(a);
    };
    // The running interaction's icon is lit when it is a top-level one, its sub-interaction's when
    // it is nested.
    if (person->running) {
        add(*person->running, !person->running->nested, person->actionIconObject, person->actionIconGuid, person->actionIconIndex, person->actionString);
    }
    if (person->runningSub) {
        add(*person->runningSub, person->runningSub->nested, person->runningSub->icon(), person->runningSub->iconGuid, person->runningSub->iconIndex,
            person->runningSub->name);
    }
    for (const auto& q : person->queue) {
        add(q, false, q.icon(), q.iconGuid, q.iconIndex, q.name);
    }
    return out;
}

bool LotLife::cancelInteraction(std::int16_t sim, std::uint32_t id) {
    auto* person = sim_->world().find(sim);
    if (person == nullptr || !ts2::sim::cancelInteraction(sim_->world(), *person, id)) {
        return false;
    }
    ts2::diag::step(std::format("Sim {} is told to stop interaction {}", sim, id));
    return true;
}

ts2::content::AgeProgress LotLife::ageProgress(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    return object ? ts2::content::ageProgress(object->personData[kAgeDuration], object->personData[kAgeDaysLeft]) : ts2::content::AgeProgress{};
}

const std::vector<ts2::swaf::Want>& LotLife::wants(std::int16_t sim) {
    static const std::vector<ts2::swaf::Want> none;
    const std::int16_t id = neighborId(sim);
    return id ? sim_->world().neighborWants(id) : none;
}

const std::vector<ts2::swaf::Want>& LotLife::fears(std::int16_t sim) {
    static const std::vector<ts2::swaf::Want> none;
    const std::int16_t id = neighborId(sim);
    return id ? sim_->world().neighborFears(id) : none;
}

bool LotLife::wantsRefilling(std::int16_t sim) {
    const std::int16_t id = neighborId(sim);
    return id && ts2::sim::wantRefillWaiting(sim_->world(), id);
}

const std::map<std::size_t, std::uint32_t>& LotLife::satisfiedWantPlaces(std::int16_t sim) {
    static const std::map<std::size_t, std::uint32_t> none;
    const std::int16_t id = neighborId(sim);
    const auto& satisfied = sim_->world().wantsState().satisfied;
    const auto it = satisfied.find(id);
    return it != satisfied.end() ? it->second : none;
}

ts2::swaf::Slots LotLife::wantSlots(std::int16_t sim) {
    const std::int16_t id = neighborId(sim);
    return id ? sim_->world().wantSlots(id) : ts2::swaf::Slots{};
}

std::optional<LotLife::NeighborName> LotLife::neighborName(std::int16_t id) {
    auto& world = sim_->world();
    const auto* neighbor = world.neighbor(id);
    if (neighbor == nullptr) {
        return std::nullopt;
    }
    auto& code = sim_->code();
    NeighborName name;
    name.female = world.neighborPersonValue(id, ts2::sim::kGenderPersonData) == 1;
    if (const auto* type = code.objectType(neighbor->guid)) {
        const auto set = static_cast<std::uint16_t>(type->definition.field(ts2::objd::field::kCatalogStringsId));
        name.first = code.catalogString(type->group, set, 0).value_or("");
        name.last = code.catalogString(type->group, set, 2).value_or("");
    }
    if (auto given = world.neighborFirstName(id)) {
        name.first = std::move(*given);
    }
    if (auto taken = world.neighborSurname(id)) {
        name.last = std::move(*taken);
    }
    return name;
}

LotLife::SimName LotLife::simName(std::int16_t sim) {
    constexpr std::size_t kTitle = 0xE6;
    auto& world = sim_->world();
    const auto* object = world.find(sim);
    if (object == nullptr) {
        return {};
    }
    SimName name;
    const std::int16_t neighbor = world.neighborIdOf(*object);
    if (auto known = neighborName(neighbor)) {
        name.first = std::move(known->first);
        name.last = std::move(known->last);
    } else if (const auto* type = sim_->code().objectType(object->definition.guid())) {
        const auto set = static_cast<std::uint16_t>(type->definition.field(ts2::objd::field::kCatalogStringsId));
        name.first = sim_->code().catalogString(type->group, set, 0).value_or("");
    }
    name.title = object->personData[kTitle];
    return name;
}

std::string LotLife::objectName(std::uint32_t guid) {
    auto& code = sim_->code();
    const auto* type = code.objectType(guid);
    if (type == nullptr) {
        return {};
    }
    const auto set = static_cast<std::uint16_t>(type->definition.field(ts2::objd::field::kCatalogStringsId));
    return code.catalogString(type->group, set, 0).value_or("");
}

void LotLife::toggleHold(std::int16_t sim, std::size_t index, bool fear) {
    constexpr std::uint8_t kHeld = 0x01;
    const std::int16_t id = neighborId(sim);
    if (id == 0) {
        return;
    }
    auto& world = sim_->world();
    auto& wants = world.neighborWants(id);
    auto& fears = world.neighborFears(id);
    auto& list = fear ? fears : wants;
    if (index >= list.size() || list[index].guid == 0) {
        return;
    }
    auto [order, fresh] = holds_.try_emplace(id);
    if (fresh) {
        for (const bool f : {false, true}) {
            for (const auto& w : f ? fears : wants) {
                if (w.flags & kHeld) {
                    order->second.push_back({f, w.guid});
                }
            }
        }
    }
    auto& held = order->second;
    auto& want = list[index];
    want.flags = static_cast<std::uint8_t>(want.flags ^ kHeld);
    const Hold hold{fear, want.guid};
    // Holds on wants fulfilled or gone since no longer count.
    std::erase_if(held, [&](const Hold& h) {
        const auto& from = h.fear ? fears : wants;
        return h == hold || std::none_of(from.begin(), from.end(), [&](const auto& w) { return w.guid == h.guid && (w.flags & kHeld); });
    });
    if (want.flags & kHeld) {
        held.push_back(hold);
        while (held.size() > world.wantSlots(id).holds) {
            auto& from = held.front().fear ? fears : wants;
            for (auto& w : from) {
                if (w.guid == held.front().guid) {
                    w.flags = static_cast<std::uint8_t>(w.flags & ~kHeld);
                }
            }
            held.pop_front();
            ts2::diag::step("released the oldest hold");
        }
    }
    ts2::diag::step(std::string(want.flags & kHeld ? "held " : "released ") + (fear ? "a fear" : "a want"));
}

std::int16_t LotLife::aspirationScore(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    return object ? object->personData[kAspirationScore] : 0;
}

std::span<const std::int16_t> LotLife::personData(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    return object ? std::span<const std::int16_t>(object->personData) : std::span<const std::int16_t>{};
}

std::vector<LotLife::Relationship> LotLife::relationships(std::int16_t sim) {
    constexpr std::size_t kFamily = 0x3D, kSpecies = 0xBA, kHidden = 0x44;
    auto& world = sim_->world();
    const auto* object = world.find(sim);
    const std::int16_t self = object ? world.neighborIdOf(*object) : 0;
    if (self == 0 || object->personData[kFamily] == 0) {
        return {};
    }
    std::set<std::int16_t> household, onLot;
    for (const std::int16_t member : sim_->household()) {
        if (const auto* m = world.find(member)) {
            household.insert(world.neighborIdOf(*m));
        }
    }
    for (const auto* p = world.nextAfter(0, [](const auto& o) { return o.isPerson(); }); p != nullptr;
         p = world.nextAfter(p->id, [](const auto& o) { return o.isPerson(); })) {
        onLot.insert(world.neighborIdOf(*p));
    }
    std::vector<Relationship> out;
    for (const std::int16_t other : world.neighborRelationshipTargets(self)) {
        if (other == self || world.neighbor(other) == nullptr) {
            continue;
        }
        auto value = [&](std::size_t i) { return world.neighborRelationshipValue(self, other, i); };
        auto person = [&](std::size_t i) { return world.neighborPersonValue(other, i); };
        Relationship r;
        r.neighbor = other;
        r.daily = value(0);
        r.flags = value(1);
        r.lifetime = value(2);
        r.familyTie = value(3) != 0;
        r.coins = (value(9) & 1) != 0;
        r.species = person(kSpecies);
        r.lifeStage = person(ts2::sim::kLifeStagePersonData);
        r.female = person(ts2::sim::kGenderPersonData) == 1;
        r.skip = (person(kHidden) & 1) != 0;
        r.household = household.contains(other);
        r.onLot = onLot.contains(other);
        out.push_back(r);
    }
    return out;
}

std::vector<LotLife::InventoryObject> LotLife::inventoryObjects(std::int16_t sim) {
    const auto* inventory = ownInventory(sim);
    std::vector<InventoryObject> out;
    if (inventory == nullptr) {
        return out;
    }
    constexpr std::uint16_t kShown = 0x1, kMemory = 0x2, kDeleted = 0x4;
    for (const auto& token : inventory->singular) {
        if ((token->flags & (kShown | kMemory | kDeleted)) != kShown) {
            continue;
        }
        const auto same = std::find_if(out.begin(), out.end(), [&](const InventoryObject& o) { return o.guid == token->guid; });
        if (same != out.end()) {
            ++same->count;
        } else {
            out.push_back({token->guid, 1});
        }
    }
    return out;
}

std::optional<std::int16_t> LotLife::inventoryValue(std::int16_t sim, std::uint32_t guid) {
    const auto* inventory = ownInventory(sim);
    if (inventory == nullptr) {
        return std::nullopt;
    }
    for (const auto* list : {&inventory->singular, &inventory->counted}) {
        for (const auto& token : *list) {
            if (token->guid == guid && !token->properties.empty()) {
                return token->properties[0];
            }
        }
    }
    return std::nullopt;
}

std::vector<std::int16_t> LotLife::inventoryValues(std::int16_t sim, std::uint32_t guid) {
    const auto* inventory = ownInventory(sim);
    if (inventory == nullptr) {
        return {};
    }
    for (const auto* list : {&inventory->singular, &inventory->counted}) {
        for (const auto& token : *list) {
            if (token->guid == guid) {
                return token->properties;
            }
        }
    }
    return {};
}

bool LotLife::hasCountedToken(std::int16_t sim, std::uint32_t guid) {
    const auto* inventory = ownInventory(sim);
    return inventory != nullptr &&
           std::any_of(inventory->counted.begin(), inventory->counted.end(), [&](const auto& token) { return token->guid == guid; });
}

std::vector<LotLife::Memory> LotLife::memories(std::int16_t sim) {
    // A memory token: flags 0x1 (visible) and 0x2 (memory), at least 12 values. Value 0 is the kind
    // (4 to 6 bad), values 6:5 the subject's GUID. The memory object must be of type 0x0F.
    constexpr std::uint16_t kMemoryFlags = 0x3;
    constexpr std::size_t kMemoryValues = 12;
    constexpr std::uint16_t kMemoryType = 0x0F;
    auto& world = sim_->world();
    auto& code = sim_->code();
    const auto* inventory = ownInventory(sim);
    if (inventory == nullptr) {
        return {};
    }
    // Line `line` of an object's catalog strings (0 its name, 1 its description, and for a Sim 2 its
    // last name), the women's set (the next string table) when asked for and not empty.
    auto catalog = [&](const ts2::sim::ObjectType& type, std::size_t line, bool female) {
        const auto set = static_cast<std::uint16_t>(type.definition.field(ts2::objd::field::kCatalogStringsId));
        std::string text = female ? code.catalogString(type.group, static_cast<std::uint16_t>(set + 1), line).value_or("") : "";
        return text.empty() ? code.catalogString(type.group, set, line).value_or("") : text;
    };
    auto putSubject = [](std::string text, const std::string& subject) {
        constexpr std::string_view kSubject = "$Subject";
        for (std::size_t at = text.find(kSubject); at != std::string::npos; at = text.find(kSubject, at + subject.size())) {
            text.replace(at, kSubject.size(), subject);
        }
        return text;
    };
    std::vector<Memory> out;
    for (auto it = inventory->singular.rbegin(); it != inventory->singular.rend(); ++it) {
        const auto& token = **it;
        if ((token.flags & kMemoryFlags) != kMemoryFlags || token.properties.size() < kMemoryValues) {
            continue;
        }
        const auto* type = code.objectType(token.guid);
        if (type == nullptr || type->definition.field(ts2::objd::field::kType) != kMemoryType) {
            continue;
        }
        Memory m;
        m.guid = token.guid;
        m.subject = static_cast<std::uint32_t>(static_cast<std::uint16_t>(token.properties[6])) << 16 | static_cast<std::uint16_t>(token.properties[5]);
        m.good = token.properties[0] < 4 || token.properties[0] > 6;
        m.pictureGroup = type->group;
        // The text follows the subject's gender. "$Subject" becomes the subject's name.
        const auto* subject = m.subject ? code.objectType(m.subject) : nullptr;
        bool female = false;
        if (subject != nullptr && subject->definition.field(ts2::objd::field::kType) == ts2::sim::kPersonType) {
            m.subjectNeighbor = world.neighborIdWithGuid(m.subject);
            female = m.subjectNeighbor != 0 && world.neighborPersonValue(m.subjectNeighbor, ts2::sim::kGenderPersonData) == 1;
            m.subjectFullName = catalog(*subject, 0, false) + " " + catalog(*subject, 2, false);
        }
        const std::string subjectName = subject ? catalog(*subject, 0, false) : "";
        m.name = putSubject(catalog(*type, 0, female), subjectName);
        m.description = putSubject(catalog(*type, 1, female), subjectName);
        out.push_back(std::move(m));
    }
    return out;
}

std::int16_t LotLife::lifeStage(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    return object ? object->personData[ts2::sim::kLifeStagePersonData] : 0;
}

std::array<int, 16> LotLife::motives(std::int16_t sim) {
    std::array<int, 16> out{};
    if (const auto* object = sim_->world().find(sim)) {
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = ts2::sim::wholeMotive(*object, i);
        }
    }
    return out;
}

int LotLife::mood(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    return object ? ts2::sim::wholeMotive(*object, ts2::sim::motive::kMood) : 0;
}

std::optional<ts2::tga::Pixels> LotLife::portrait(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    const auto image = object ? ts2::content::portraitImage(object->personData[ts2::sim::kLifeStagePersonData]) : std::nullopt;
    const auto data = image ? sim_->simImage(sim, *image) : std::vector<std::uint8_t>{};
    return data.empty() ? std::nullopt : ts2::image::decode(data);
}

std::int16_t LotLife::neighborId(std::int16_t sim) {
    const auto* object = sim_->world().find(sim);
    return object ? sim_->world().neighborIdOf(*object) : 0;
}

std::optional<LotLife::PortraitFile> LotLife::neighborPortrait(std::int16_t neighbor) {
    const auto* n = sim_->world().neighbor(neighbor);
    if (n == nullptr || n->personData.size() <= ts2::sim::kLifeStagePersonData) {
        return std::nullopt;
    }
    const auto image = ts2::content::portraitImage(n->personData[ts2::sim::kLifeStagePersonData]);
    auto file = image ? sim_->characterFile(neighbor) : std::nullopt;
    return file ? std::optional<PortraitFile>(PortraitFile{std::move(*file), *image}) : std::nullopt;
}

} // namespace app
