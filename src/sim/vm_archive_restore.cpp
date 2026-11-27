#include "diag/log.hpp"
#include "formats/object_archive.hpp"
#include "sim/lot_world.hpp"
#include "sim/object_reset.hpp"
#include "sim/vm_execution.hpp"

#include <algorithm>
#include <iterator>
#include <map>

namespace ts2::sim::vm_detail {

namespace {

constexpr std::size_t kObjectIdField = 0x0B, kContainerField = 0x02, kSlotField = 0x03;

// The objects an archived object is made of: a multi-tile object's parts, lead first, or the object
// alone. Each starts with its type's counts, out of the world. None for a multi-tile master without
// a lead part.
std::vector<SimObject*> makeArchived(World& world, CodeSource& code, const ObjectType& type) {
    std::vector<const ObjectType*> types{&type};
    if (type.definition.isMultiTileMaster()) {
        types = code.multiTileParts(type);
        if (types.empty()) {
            return {};
        }
    }
    std::vector<SimObject*> made;
    for (const ObjectType* t : types) {
        SimObject& object = world.create(t->group, t->definition, containerSlotsOf(code, *t));
        object.data[kTargetSlotCountField] = targetSlotCount(*t);
        object.data[kSemiAttributeCountField] = semiAttributeCount(code, *t);
        made.push_back(&object);
    }
    return made;
}

} // namespace

// Restores the objects of the neighborhood's object archive `archive`, as the game does when a token
// that names an archive is made into an object again. Each archived object is made from its type
// (a multi-tile one with all its parts, its saved parts loaded into them in order) and given its
// saved state. Ids it saved are then mapped to the new ones: its own id and lead, its container
// (object data 0x02, 0x5A and 0x5F) and what its slots hold, an id not in the archive becoming 0.
// Every object is taken out of the world with its stack restarted at Main and runs its Load function
// (OBJf entry 2) with `params`. The first object, outside any container, faces direction 2 last.
// Returns its id, 0 when the archive is missing or its first object can't be made.
std::int16_t Execution::restoreArchive(std::uint32_t archiveId, std::span<const std::int16_t> params) {
    const auto bytes = archiveId >= 1 ? world().archive(archiveId) : std::nullopt;
    const auto record = bytes ? archive::readRecord(*bytes) : std::nullopt;
    if (!record || record->objects.empty()) {
        return 0;
    }
    std::vector<std::vector<SimObject*>> objects;  // each archived object's parts, lead first
    std::map<std::int16_t, std::int16_t> newIds;   // saved id -> new id
    for (const archive::Object& archived : record->objects) {
        const ObjectType* type = code_.objectType(archived.guid);
        std::vector<SimObject*> parts = type != nullptr ? makeArchived(world(), code_, *type) : std::vector<SimObject*>{};
        if (parts.empty()) {
            // The game makes Placeholder objects for a type it doesn't have, which isn't ported yet.
            diag::warnOnce("simulation: an archived object whose type is missing or has no lead part is left out");
            if (objects.empty()) {
                return 0;
            }
            continue;
        }
        for (std::size_t i = 0; i < parts.size() && i < archived.parts.size(); ++i) {
            const ObjectType* partType = code_.objectType(parts[i]->definition.guid());
            for (const std::string& problem : applySavedState(*parts[i], archived.parts[i].state, code_, partType ? *partType : *type)) {
                diag::warnOnce("simulation: an archived object: " + problem);
            }
            newIds[archived.parts[i].state.data[kObjectIdField]] = parts[i]->id;
        }
        objects.push_back(std::move(parts));
    }
    const auto remapped = [&](std::int16_t saved) {
        const auto it = newIds.find(saved);
        return it != newIds.end() ? it->second : std::int16_t{0};
    };
    constexpr std::size_t kSlotOwnerFields[] = {kContainerField, 0x5A, 0x5F}, kClearedFields[] = {0x19, 0x3E};
    for (const auto& parts : objects) {
        for (SimObject* part : parts) {
            part->data[kObjectIdField] = part->id;
            part->data[kLeadTileField] = parts.size() > 1 ? parts.front()->id : part->id;
            for (const std::size_t field : kSlotOwnerFields) {
                part->data[field] = remapped(part->data[field]);
            }
            for (std::int16_t& held : part->slots) {
                held = held != 0 ? remapped(held) : std::int16_t{0};
            }
            for (const std::size_t field : kClearedFields) {
                part->data[field] = 0;
            }
            // Objects with this tree are told each id change, which isn't ported yet.
            if (code_.behaviorByName(part->group, "CT - Handle Object ID Change")) {
                diag::warnOnce("simulation: a restored object's \"CT - Handle Object ID Change\" isn't run yet");
            }
        }
    }
    SimObject& first = *objects.front().front();
    first.data[kContainerField] = 0;
    first.data[kSlotField] = -1;
    constexpr std::size_t kPlacementFlagsField = 0x08, kUseCountField = 0x3E;
    constexpr std::int16_t kPlacedFlags = 0x120;
    for (const auto& parts : objects) {
        for (SimObject* part : parts) {
            part->data[kPlacementFlagsField] &= static_cast<std::int16_t>(~kPlacedFlags);
            part->data[kUseCountField] = 0;
            part->data[kRoomDataField] = -1;
            part->x = kOffLot;
            part->y = kOffLot;
            part->level = 1;
            part->tile.reset();
            if (const ObjectType* type = code_.objectType(part->definition.guid())) {
                restartMain(*part, *type, code_);
            }
        }
    }
    constexpr std::uint16_t kLoad = 2;  // OBJf entry
    for (const auto& parts : objects) {
        for (SimObject* part : parts) {
            const ObjectType* type = code_.objectType(part->definition.guid());
            const auto* functions = type ? code_.objectFunctions(*type) : nullptr;
            if (functions != nullptr && kLoad < functions->size() && (*functions)[kLoad].action != 0 && runOnObject) {
                runOnObject(*part, type->group, (*functions)[kLoad].action, 0, params);
            }
        }
    }
    // Turned to face direction 2, its parts and what its slots hold by the same turn.
    constexpr std::int16_t kRestoredFacing = 2;
    const auto turn = static_cast<std::int16_t>(((10 - kRestoredFacing) & 7) - first.data[kDirectionField]);
    for (SimObject* part : objects.front()) {
        part->data[kDirectionField] = static_cast<std::int16_t>((part->data[kDirectionField] + turn) & 7);
        for (const std::int16_t held : part->slots) {
            if (SimObject* h = held > 0 ? world().find(held) : nullptr) {
                h->data[kDirectionField] = static_cast<std::int16_t>((h->data[kDirectionField] + turn) & 7);
            }
        }
    }
    return first.id;
}

// A new object of a type, out of the world, started as Create New Object Instance starts one (Init,
// then Main). None for a type the game doesn't make this way: Sims and OBJD types 0x10 and 0x11.
std::int16_t Execution::createOutOfWorld(std::uint32_t guid) {
    const ObjectType* type = code_.objectType(guid);
    constexpr std::uint16_t kSimType = 2, kRefusedTypes[] = {kSimType, 0x10, 0x11};
    if (type == nullptr ||
        std::find(std::begin(kRefusedTypes), std::end(kRefusedTypes), type->definition.field(objd::field::kType)) != std::end(kRefusedTypes)) {
        return 0;
    }
    std::vector<SimObject*> made = makeArchived(world(), code_, *type);
    if (made.empty()) {
        return 0;
    }
    for (SimObject* part : made) {
        stampBirth(*part);
        part->data[kRoomDataField] = -1;
        part->data[kLeadTileField] = made.size() > 1 ? made.front()->id : part->data[kLeadTileField];
    }
    if (made.size() > 1) {
        constexpr std::size_t kCurrentValueField = 0x29;
        made.front()->data[kCurrentValueField] = static_cast<std::int16_t>(type->definition.field(objd::field::kPrice));
        if (startCreatedParts) {
            startCreatedParts(made);
        }
    } else {
        if (startCreated) {
            startCreated(*made.front(), *type);
        }
        giveValue(*made.front());
    }
    return made.front()->id;
}

} // namespace ts2::sim::vm_detail
