#include "sim/vm_execution.hpp"

namespace ts2::sim::vm_detail {

// Search 0x26 (no Edith name): the next object type, after the one the target counts (from 1), whose
// OBJD words 0x4B and 0x4C equal temps 0 and 1 (a negative temp matches any): the target then
// counts it and temps 2 and 3 hold its GUID's low and high halves. The radio finds its stations
// this way (word 0x4B 117, 0x4C the station number). The filters don't apply.
bool Execution::nextObjectTypeByFields(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f) {
    constexpr std::uint16_t kKindWord = 0x4B, kNumberWord = 0x4C;
    const std::int16_t kind = thread_.temps[0], number = thread_.temps[1];
    const auto& guids = code_.objectTypeGuids();
    const std::int16_t target = read(targetOwner, targetData, f);
    for (std::size_t i = target > 0 ? static_cast<std::size_t>(target) : 0; i < guids.size(); ++i) {
        const ObjectType* type = code_.objectType(guids[i]);
        if (type == nullptr) {
            continue;
        }
        const auto word = [&](std::uint16_t at) { return static_cast<std::int16_t>(type->definition.field(at)); };
        if (kind < 0 || (word(kKindWord) == kind && (number < 0 || word(kNumberWord) == number))) {
            if (i + 1 > static_cast<std::size_t>(INT16_MAX)) {
                return false;
            }
            write(targetOwner, targetData, f, static_cast<std::int16_t>(i + 1));
            thread_.temps[2] = static_cast<std::int16_t>(guids[i] & 0xFFFF);
            thread_.temps[3] = static_cast<std::int16_t>(guids[i] >> 16);
            return true;
        }
    }
    return false;
}

// Search 0x0E: the target (a Sim) becomes the first object of its job's type on the lot, or 0 when
// there is none. A Sim without a job (an unknown type) is false and keeps the target.
bool Execution::jobObject(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f) {
    const std::int16_t simId = read(targetOwner, targetData, f);
    const SimObject* sim = world().find(simId);
    if (sim == nullptr || !sim->isPerson()) {
        throw Stop{Outcome::Error, "object referred to must be a person", std::to_string(simId)};
    }
    static_assert(kJobGuidHighPersonData < kPersonDataCount && kJobGuidLowPersonData < kPersonDataCount);
    const std::uint32_t job =
        static_cast<std::uint32_t>(static_cast<std::uint16_t>(sim->personData[kJobGuidHighPersonData])) << 16 |
        static_cast<std::uint16_t>(sim->personData[kJobGuidLowPersonData]);
    if (code_.objectType(job) == nullptr) {
        return false;
    }
    const SimObject* first = world().nextAfter(0, [job](const SimObject& s) { return s.definition.guid() == job; });
    write(targetOwner, targetData, f, first != nullptr ? first->id : std::int16_t{0});
    return true;
}

// Operands:
//   bytes 0-3    GUID
//   byte 4       search type, bit 0x80 = explicit target
//   byte 5, 7    target (owner, index)
//   byte 8       filter flags
//   bytes 9-10   object data field that must equal bytes 11-12 (with flag bit 1)
bool Execution::setToNext(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const std::uint32_t guid = readGuid(o);
    const std::uint8_t searchType = o[4] & 0x7F;
    std::uint8_t targetOwner = o[5];
    std::uint16_t targetData = o[7];
    if ((o[4] & 0x80) == 0) {
        targetOwner = owner::kStackObjectId;
        targetData = 0;
    }

    switch (searchType) {
    case kVerifyStackObject:
        return world().find(read(targetOwner, targetData, f)) != nullptr;
    case kVerifySimInStackObject: {
        const SimObject* object = world().find(read(targetOwner, targetData, f));
        return object != nullptr && object->isPerson();
    }
    case kVerifyNeighborInStackObject: {
        // The target holds a neighbor id. True when the neighborhood has that neighbor.
        const auto& neighbors = world().neighbors();
        if (!neighbors) {
            throw Stop{Outcome::Unsupported, "Set to Next needs the neighborhood", hex(searchType, 2)};
        }
        return neighbors->contains(read(targetOwner, targetData, f));
    }
    case kSearchSameTile:
        return nextOnSameTile(o, targetOwner, targetData, f);
    case kSearchMultiTilePart:
        return nextMultiTilePart(targetOwner, targetData, f);
    case kSearchObjectOwner:
        return nextObjectOwner(targetOwner, targetData, f);
    case kSearchVehicle:
        return nextVehicle(targetOwner, targetData, f);
    case kSearchAdjacent:
        return nextAdjacent(o, targetOwner, targetData, f);
    case kSearchFamilyTie:
        return nextFamilyTie(targetOwner, targetData, f);
    case kSearchRelationshipFlags:
        return nextWithRelationshipFlags(targetOwner, targetData, f);
    case kSearchRelationshipAbove:
        return nextWithRelationshipAbove(targetOwner, targetData, f);
    case kSearchObjectTypeByFields:
        return nextObjectTypeByFields(targetOwner, targetData, f);
    case kSearchJobObject:
        return jobObject(targetOwner, targetData, f);
    case kSearchNeighborOfType:
        return nextNeighborOfType(guid, targetOwner, targetData, f);
    case kSearchParents:
    case kSearchSiblings:
    case kSearchChildren:
    case kSearchSpouse:
        return nextRelative(searchType, targetOwner, targetData, f);
    default:
        break;
    }

    const std::uint8_t flags = o[8];
    const bool requireField = (flags & 0x02) != 0;
    const auto fieldIndex = static_cast<std::uint16_t>(o[9] | o[10] << 8);
    const auto fieldValue = static_cast<std::int16_t>(o[11] | o[12] << 8);
    if (requireField && fieldIndex >= kObjectDataCount) {
        throw Stop{Outcome::Error, "object data index out of range", std::to_string(fieldIndex)};
    }
    auto passesFilters = [&](const SimObject& s) {
        if ((flags & 0x01) == 0 && (s.data[kSearchSkipFlagsField] & 0x05) != 0) {
            return false;
        }
        if ((flags & 0x04) == 0 && s.data[kSearchSkipField] != 0) {
            return false;
        }
        return !requireField || s.data[fieldIndex] == fieldValue;
    };

    if (searchType == kSearchInView) {
        return nextInView(passesFilters, targetOwner, targetData, f);
    }
    if (searchType == kSearchHumanNeighbor || (searchType >= kSearchAnyNeighbor && searchType <= kSearchSpecies3Neighbor)) {
        return nextNeighbor(searchType, passesFilters, targetOwner, targetData, f);
    }
    if (searchType == kSearchFamilyNeighbor) {
        const std::int16_t family = thread_.temps[0];
        return nextNeighborWhere([family](const std::vector<std::int16_t>& pd) {
            return kFamilyPersonData < pd.size() && pd[kFamilyPersonData] == family;
        }, passesFilters, targetOwner, targetData, f, searchType);
    }

    auto speciesIs = [](auto accept) {
        return [accept](const SimObject& s) {
            return s.isPerson() && accept(s.personData[kSpeciesPersonData]);
        };
    };
    std::function<bool(const SimObject&)> matches;
    switch (searchType) {
    case kSearchObject:        matches = [](const SimObject&) { return true; }; break;
    case kSearchNonPerson:     matches = [](const SimObject& s) { return !s.isPerson(); }; break;
    case kSearchObjectOfType:
    case kSearchObjectOfTypeInTemps: {
        const std::uint32_t type = searchType == kSearchObjectOfType
                                       ? guid
                                       : static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[0])) |
                                             static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[1])) << 16;
        // An unknown object type is false and leaves the target as it is.
        if (code_.objectType(type) == nullptr) {
            return false;
        }
        matches = [type](const SimObject& s) { return s.definition.guid() == type; };
        break;
    }
    case kSearchObjectInRoomInTemp0: {
        const std::int16_t room = thread_.temps[0];
        matches = [room](const SimObject& s) { return s.data[kRoomDataField] == room; };
        break;
    }
    case kSearchHauntable:
        matches = [](const SimObject& s) { return s.definition.field(objd::field::kHauntable) != 0; };
        break;
    case kSearchPerson:        matches = speciesIs([](std::int16_t v) { return v == 0; }); break;
    case kSearchAnySpecies:    matches = speciesIs([](std::int16_t) { return true; }); break;
    case kSearchNonHuman:      matches = speciesIs([](std::int16_t v) { return v != 0; }); break;
    case kSearchSpecies1Or2:   matches = speciesIs([](std::int16_t v) { return v == 1 || v == 2; }); break;
    case kSearchSpecies1:      matches = speciesIs([](std::int16_t v) { return v == 1; }); break;
    case kSearchSpecies2:      matches = speciesIs([](std::int16_t v) { return v == 2; }); break;
    case kSearchSpecies3:      matches = speciesIs([](std::int16_t v) { return v == 3; }); break;
    case kSearchCategoryInTemp0:
    case kSearchCategoryInParam0: {
        const std::int16_t category = searchType == kSearchCategoryInTemp0 ? thread_.temps[0]
                                      : f.params.empty()                   ? std::int16_t{0}
                                                                           : f.params[0];
        matches = [category](const SimObject& s) { return s.data[kCategoryField] == category; };
        break;
    }
    default:
        throw Stop{Outcome::Unsupported, "Set to Next search type not implemented",
                   hex(searchType, 2)};
    }

    SimObject* next = world().nextAfter(read(targetOwner, targetData, f), [&](const SimObject& s) {
        return matches(s) && passesFilters(s);
    });
    if (next == nullptr) {
        write(targetOwner, targetData, f, 0);
        return false;
    }
    write(targetOwner, targetData, f, next->id);
    return true;
}

// Search 0x18. A target the list doesn't have, or the end of the list, is 0 and false. A Sim off
// the lot finds nothing and leaves the target as it is.
template <typename Filters>
bool Execution::nextInView(const Filters& passesFilters, std::uint8_t targetOwner, std::uint16_t targetData, Frame& f) {
    SimObject& sim = me();
    if (!sim.isPerson()) {
        throw Stop{Outcome::Error, "object referred to must be a person", std::to_string(sim.id)};
    }
    const std::int16_t range = thread_.temps[0];
    std::int16_t target = read(targetOwner, targetData, f);
    if (!sim.tile) {
        return false;
    }
    if (sim.inViewStale && target == 0) {
        sim.inView = objectsInView(world(), sim, range);
        sim.inViewStale = false;
    }
    const auto& ids = sim.inView;
    std::size_t next = 0;
    if (target != 0) {
        const auto at = std::find(ids.begin(), ids.end(), target);
        if (at == ids.end()) {
            write(targetOwner, targetData, f, 0);
            return false;
        }
        next = static_cast<std::size_t>(at - ids.begin()) + 1;
    }
    // After a rejected object the search goes on from the list, not from the target read back
    // (some owners store a different value than they are given).
    for (;;) {
        // The game steps past objects that left the lot, except the list's last one.
        const SimObject* found = nullptr;
        while (next < ids.size() && (found == nullptr || !found->tile)) {
            target = ids[next++];
            found = world().find(target);
        }
        if (found == nullptr) {
            write(targetOwner, targetData, f, 0);
            return false;
        }
        write(targetOwner, targetData, f, target);
        if (passesFilters(*found)) {
            return true;
        }
    }
}

// Search 0x07: the next neighbor id after the target whose Sim is of the type with the operands'
// GUID. When that type is an NPC template (OBJD type 0x10), a neighbor matches whose type was
// cloned from it (OBJD fields 0x46-0x47). The walk passes over family-tree-only neighbors and
// writes each id it reaches into the target, 0 when it runs out (false). An unknown GUID is false
// after the first step. The filters don't apply.
bool Execution::nextNeighborOfType(std::uint32_t guid, std::uint8_t targetOwner, std::uint16_t targetData, Frame& f) {
    constexpr std::uint16_t kTemplateType = 0x10, kFamilyTreeOnlyType = 0x11;
    constexpr std::size_t kOriginalGuidLowField = 0x46, kOriginalGuidHighField = 0x47;
    const auto* ids = world().neighbors();
    if (ids == nullptr) {
        throw Stop{Outcome::Unsupported, "Set to Next needs the neighborhood", hex(kSearchNeighborOfType, 2)};
    }
    auto typeOf = [&](std::int16_t id) {
        const SavedNeighbor* n = world().neighbor(id);
        return n != nullptr ? code_.objectType(n->guid) : nullptr;
    };
    auto step = [&](std::int16_t after) -> std::int16_t {
        for (auto it = ids->upper_bound(after); it != ids->end(); ++it) {
            const ObjectType* type = typeOf(*it);
            if (type == nullptr || type->definition.field(objd::field::kType) != kFamilyTreeOnlyType) {
                write(targetOwner, targetData, f, *it);
                return *it;
            }
        }
        write(targetOwner, targetData, f, 0);
        return 0;
    };
    std::int16_t target = step(read(targetOwner, targetData, f));
    const ObjectType* wanted = code_.objectType(guid);
    if (wanted == nullptr) {
        return false;
    }
    const bool byTemplate = wanted->definition.field(objd::field::kType) == kTemplateType;
    for (; target != 0; target = step(target)) {
        const SavedNeighbor* n = world().neighbor(target);
        const ObjectType* type = typeOf(target);
        std::optional<std::uint32_t> value;
        if (byTemplate && type != nullptr) {
            value = type->definition.field(kOriginalGuidLowField) |
                    static_cast<std::uint32_t>(type->definition.field(kOriginalGuidHighField)) << 16;
        } else if (!byTemplate && n != nullptr) {
            value = n->guid;
        }
        if (value == guid) {
            return true;
        }
    }
    return false;
}

// Next neighbor id after the target whose species fits the search.
template <typename Filters>
bool Execution::nextNeighbor(std::uint8_t searchType, const Filters& passesFilters, std::uint8_t targetOwner,
                  std::uint16_t targetData, Frame& f) {
    auto speciesFits = [searchType](const std::vector<std::int16_t>& personData) {
        const std::int16_t species = personData[kSpeciesPersonData];
        switch (searchType) {
        case kSearchHumanNeighbor:       return species == 0;
        case kSearchAnyNeighbor:         return true;
        case kSearchNonHumanNeighbor:    return species != 0;
        case kSearchSpecies1Or2Neighbor: return species == 1 || species == 2;
        case kSearchSpecies1Neighbor:    return species == 1;
        case kSearchSpecies2Neighbor:    return species == 2;
        case kSearchSpecies3Neighbor:    return species == 3;
        default:                         return false;
        }
    };
    return nextNeighborWhere(speciesFits, passesFilters, targetOwner, targetData, f, searchType);
}

// Next neighbor id after the target whose person data fits. A neighbor whose Sim is on the lot
// must also pass the search filters. One whose Sim isn't here always qualifies. Running out
// leaves the target at 0 and is false.
template <typename Fits, typename Filters>
bool Execution::nextNeighborWhere(const Fits& fits, const Filters& passesFilters, std::uint8_t targetOwner,
                       std::uint16_t targetData, Frame& f, std::uint8_t searchType) {
    const auto* ids = world().neighbors();
    if (ids == nullptr) {
        throw Stop{Outcome::Unsupported, "Set to Next needs the neighborhood", hex(searchType, 2)};
    }
    for (auto it = ids->upper_bound(read(targetOwner, targetData, f)); it != ids->end(); ++it) {
        const auto* personData = world().neighborPersonData(*it);
        if (personData == nullptr || !fits(*personData)) {
            continue;
        }
        const SavedNeighbor* neighbor = world().neighbor(*it);
        const SimObject* sim = world().nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& s) {
            return s.isPerson() && s.definition.guid() == neighbor->guid;
        });
        if (sim == nullptr || passesFilters(*sim)) {
            write(targetOwner, targetData, f, *it);
            return true;
        }
    }
    write(targetOwner, targetData, f, 0);
    return false;
}

// The family of the neighbor in temp 0 (searches 0x15, 0x16, 0x17, 0x1F). Siblings and children:
// the next one after the target in the record's order, none left being false with the target 0.
// Parents: father, then mother, then 0 (false). Spouse: always the spouse. Only neighbors still
// in the neighborhood count.
bool Execution::nextRelative(std::uint8_t searchType, std::uint8_t targetOwner, std::uint16_t targetData, Frame& f) {
    const auto* ids = world().neighbors();
    if (ids == nullptr) {
        throw Stop{Outcome::Unsupported, "Set to Next needs the neighborhood", hex(searchType, 2)};
    }
    const std::int16_t of = thread_.temps[0];
    if (!ids->contains(of)) {
        throw Stop{Outcome::Error, "neighbor not found", std::to_string(of)};
    }
    const auto present = [&](std::int16_t id) { return id > 0 && ids->contains(id); };
    const bool known = world().hasFamilyTies(of);
    const std::int16_t current = read(targetOwner, targetData, f);
    const auto first = [&](famt::TieType type) {
        const auto ties = world().familyTies(of, type);
        return ties.empty() ? std::int16_t{0} : ties.front();
    };
    if (searchType == kSearchSpouse) {
        const std::int16_t spouse = first(famt::TieType::Spouse);
        if (!known || !present(spouse)) {
            return false;
        }
        write(targetOwner, targetData, f, spouse);
        return true;
    }
    if (searchType == kSearchParents) {
        if (!known) {
            return false;
        }
        const std::int16_t father = first(famt::TieType::Father), mother = first(famt::TieType::Mother);
        if (current == father) {
            write(targetOwner, targetData, f, mother);
            return present(mother);
        }
        if (current == mother && mother != 0) {
            write(targetOwner, targetData, f, 0);
            return false;
        }
        write(targetOwner, targetData, f, father);
        return present(father);
    }
    const auto list = world().familyTies(of, searchType == kSearchSiblings ? famt::TieType::Sibling : famt::TieType::Child);
    std::size_t from = 0;
    if (present(current)) {
        if (const auto it = std::find(list.begin(), list.end(), current); it != list.end()) {
            from = static_cast<std::size_t>(it - list.begin()) + 1;
        }
    }
    for (std::size_t i = from; i < list.size(); ++i) {
        if (present(list[i])) {
            write(targetOwner, targetData, f, list[i]);
            return true;
        }
    }
    write(targetOwner, targetData, f, 0);
    return false;
}

// Next neighbor id after the target's (ascending) towards whom the neighbor in temp 0 holds a
// family tie (relationship variable 3 not 0). None left is false with the target 0. The game
// also skips neighbors of one kind not identified yet.
bool Execution::nextFamilyTie(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f) {
    const auto* ids = world().neighbors();
    if (ids == nullptr) {
        throw Stop{Outcome::Unsupported, "Set to Next needs the neighborhood", hex(kSearchFamilyTie, 2)};
    }
    const std::int16_t of = thread_.temps[0];
    if (!ids->contains(of)) {
        throw Stop{Outcome::Error, "neighbor not found", std::to_string(of)};
    }
    for (auto it = ids->upper_bound(read(targetOwner, targetData, f)); it != ids->end(); ++it) {
        if (*it > 0 && world().neighborRelationshipValue(of, *it, kFamilyTieRelationship) != 0) {
            write(targetOwner, targetData, f, *it);
            return true;
        }
    }
    write(targetOwner, targetData, f, 0);
    return false;
}

// Search 0x24, walked like 0x1C. The search filters don't apply.
bool Execution::nextWithRelationshipFlags(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f) {
    const auto* ids = world().neighbors();
    if (ids == nullptr) {
        throw Stop{Outcome::Unsupported, "Set to Next needs the neighborhood", hex(kSearchRelationshipFlags, 2)};
    }
    const std::int16_t of = thread_.temps[0];
    if (!ids->contains(of)) {
        throw Stop{Outcome::Error, "neighbor not found", std::to_string(of)};
    }
    for (auto it = ids->upper_bound(read(targetOwner, targetData, f)); it != ids->end(); ++it) {
        const std::int16_t flags = world().neighborRelationshipValue(of, *it, kRelationshipFlagsVariable);
        if (*it > 0 && (flags & thread_.temps[1]) != 0) {
            write(targetOwner, targetData, f, *it);
            return true;
        }
    }
    write(targetOwner, targetData, f, 0);
    return false;
}

// Set to Next search 0x23, walked like search 0x1C: only the neighbors that A (the neighbor in
// temp 0) has relationship variables toward count, and the usual search filters don't apply.
bool Execution::nextWithRelationshipAbove(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f) {
    const auto* ids = world().neighbors();
    if (ids == nullptr) {
        throw Stop{Outcome::Unsupported, "Set to Next needs the neighborhood", hex(kSearchRelationshipAbove, 2)};
    }
    const std::int16_t of = thread_.temps[0];
    if (!ids->contains(of)) {
        throw Stop{Outcome::Error, "neighbor not found", std::to_string(of)};
    }
    for (auto it = ids->upper_bound(read(targetOwner, targetData, f)); it != ids->end(); ++it) {
        if (*it > 0 && world().neighborRelationshipSize(of, *it) > 0 &&
            world().neighborRelationshipValue(of, *it, 0) > thread_.temps[1]) {
            write(targetOwner, targetData, f, *it);
            return true;
        }
    }
    write(targetOwner, targetData, f, 0);
    return false;
}

// Next object beside the object in local [byte 6], in its room: the first object on the tile
// at +x, then +y, -x and -y, continuing after the side the target is on. Running out is false
// and leaves the target as it is.
bool Execution::nextAdjacent(const std::array<std::uint8_t, 16>& o, std::uint8_t targetOwner, std::uint16_t targetData,
                  Frame& f) {
    const std::uint8_t local = o[6];
    if (local >= f.locals.size()) {
        throw Stop{Outcome::Error, "stack number out of range", std::to_string(local)};
    }
    const SimObject* reference = world().find(f.locals[local]);
    if (reference == nullptr) {
        throw Stop{Outcome::Error, "reference to tree parameter when no parameter exists",
                   std::to_string(f.locals[local])};
    }
    if (!reference->tile) {
        return false;
    }
    const Tile origin = *reference->tile;
    auto sign = [](int v) { return (v > 0) - (v < 0); };
    int dx = 1;
    int dy = 0;
    bool advance = false;
    if (const SimObject* current = world().find(read(targetOwner, targetData, f)); current && current->tile) {
        dx = sign(current->tile->x - origin.x);
        dy = sign(current->tile->y - origin.y);
        advance = true;
    }
    for (;;) {
        if (advance) {
            if (dx >= 1) {
                dx = 0;
                dy = 1;
            } else if (dx < 0) {
                dx = 0;
                dy = -1;
            } else if (dy < 1) {
                return false;
            } else {
                dx = -1;
                dy = 0;
            }
        }
        advance = true;
        const Tile side{static_cast<std::uint8_t>(origin.x + dx), static_cast<std::uint8_t>(origin.y + dy),
                        origin.level};
        // The game takes the first object of the tile's own list. We use the lowest id.
        const SimObject* candidate = world().nextAfter(std::numeric_limits<std::int16_t>::min(),
                                                       [&](const SimObject& s) { return s.tile == side; });
        if (candidate != nullptr && candidate->data[kRoomDataField] == reference->data[kRoomDataField]) {
            write(targetOwner, targetData, f, candidate->id);
            return true;
        }
    }
}

// Next part of the target's multi-tile object, in falling id order and wrapping around (the
// target itself when it is the only part). Not a multi-tile object: false, and the target becomes 0.
bool Execution::nextMultiTilePart(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f) {
    const std::int16_t targetId = read(targetOwner, targetData, f);
    const SimObject* target = world().find(targetId);
    if (target == nullptr || target->definition.field(objd::field::kMultiTileMasterId) == 0) {
        write(targetOwner, targetData, f, 0);
        return false;
    }
    const std::int16_t lead = target->data[kLeadTileObjectField];
    const SimObject* next = world().previousAround(targetId, [&](const SimObject& s) {
        return s.id == targetId || (lead > 0 && s.data[kLeadTileObjectField] == lead &&
                                    s.definition.field(objd::field::kMultiTileMasterId) != 0);
    });
    write(targetOwner, targetData, f, next->id);
    return true;
}

// Search 0x20: the next id the object in temp 0 holds a relationship record towards, after the
// target (from the first when the target is below 1), whose record has a variable 1 of at least
// temp 1. The game compares them without sign. Running out, or a target the object has no record
// towards, leaves -1 and is false. A missing object is false and leaves the target alone. The
// game walks its records in its own hash order, taken as id order here.
bool Execution::nextObjectOwner(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f) {
    const SimObject* object = world().find(thread_.temps[0]);
    if (object == nullptr) {
        return false;
    }
    const std::vector<std::int16_t> ids = world().relationshipTargets(object->id);
    const std::int16_t target = read(targetOwner, targetData, f);
    auto it = ids.begin();
    if (target >= 1) {
        it = std::find(ids.begin(), ids.end(), target);
        if (it != ids.end()) {
            ++it;
        }
    }
    const auto threshold = static_cast<std::uint32_t>(static_cast<std::int32_t>(thread_.temps[1]));
    for (; it != ids.end(); ++it) {
        const std::vector<std::int16_t>& values = world().relationship(object->id, *it);
        if (values.size() > 1 && static_cast<std::uint32_t>(static_cast<std::int32_t>(values[1])) >= threshold) {
            write(targetOwner, targetData, f, *it);
            return true;
        }
    }
    write(targetOwner, targetData, f, -1);
    return false;
}

// Search 0x10: the game keeps a list of the objects of OBJD type 0x0D or 0x12 (vehicles). The next
// one after the target (the first when the target is 0) that isn't removed. A target not in the
// list, or running out, leaves 0 and is false. The search filters don't apply. The game's list is
// in the order objects were added, taken as id order here.
bool Execution::nextVehicle(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f) {
    constexpr std::uint16_t kVehicleType = 0x0D, kOtherVehicleType = 0x12;
    const std::vector<std::int16_t> ids = world().idsIncludingRemoved([](const SimObject& s) {
        const std::uint16_t type = s.definition.field(objd::field::kType);
        return type == kVehicleType || type == kOtherVehicleType;
    });
    const std::int16_t target = read(targetOwner, targetData, f);
    auto it = ids.begin();
    if (target != 0) {
        it = std::find(ids.begin(), ids.end(), target);
        if (it != ids.end()) {
            ++it;
        }
    }
    for (; it != ids.end(); ++it) {
        if (world().find(*it) != nullptr) {
            write(targetOwner, targetData, f, *it);
            return true;
        }
    }
    write(targetOwner, targetData, f, 0);
    return false;
}

// Next object on the target's tile, wrapping around (the target itself when it is alone).
// The game keeps its own order per tile, which isn't saved. We use id order.
bool Execution::nextOnSameTile(const std::array<std::uint8_t, 16>& o, std::uint8_t targetOwner,
                    std::uint16_t targetData, Frame& f) {
    const std::int16_t targetId = read(targetOwner, targetData, f);
    const SimObject* target = targetId != 0 ? world().find(targetId) : nullptr;
    if (target == nullptr || !target->tile) {
        return false;
    }
    const std::uint8_t flags = o[8];
    const auto fieldIndex = static_cast<std::uint16_t>(o[9] | o[10] << 8);
    const auto fieldValue = static_cast<std::int16_t>(o[11] | o[12] << 8);
    auto passesFilters = [&](const SimObject& s) {
        if ((flags & 0x01) == 0 && (s.data[kSearchSkipFlagsField] & 0x05) != 0) {
            return false;
        }
        // The game reads a field past the end as 0 here.
        const std::int16_t value = fieldIndex < kObjectDataCount ? s.data[fieldIndex] : 0;
        return (flags & 0x02) == 0 || value == fieldValue;
    };
    if (!passesFilters(*target)) {
        return false;
    }
    if (target->data[kSameTileRefusedField] & kSameTileRefusedBit) {
        throw Stop{Outcome::Error, "could not find object reference", std::to_string(targetId)};
    }
    const Tile tile = *target->tile;
    const SimObject* next = world().nextAround(targetId, [&](const SimObject& s) {
        return s.id == targetId || (s.tile == tile && passesFilters(s));
    });
    write(targetOwner, targetData, f, next->id);
    return true;
}

} // namespace ts2::sim::vm_detail
