#include "sim/vm_execution.hpp"

namespace ts2::sim::vm_detail {

bool Execution::genericSimsCall(const bhav::Instruction& in) {
    const std::uint8_t call = in.operands[0];
    if (std::find(std::begin(kNoOpGenericCalls), std::end(kNoOpGenericCalls), call) !=
        std::end(kNoOpGenericCalls)) {
        return true;
    }
    if (call == kInitObjectGenericCall) {
        initObjectDefaults(me());
        return true;
    }
    if (call == kUpdateFootprintGenericCall) {
        if (world().find(stackObject()) == nullptr) {
            throw Stop{Outcome::Error, "stack object does not exist", std::to_string(stackObject())};
        }
        world().markFootprintChanged(stackObject());
        return true;
    }
    if (call == kOkToIdleHereGenericCall) {
        return okToIdleHere(world(), me());
    }
    if (call == kWallInFrontGenericCall) {
        const auto room = wallInFront(world(), me());
        if (!room) {
            return false;
        }
        thread_.temps[1] = static_cast<std::int16_t>(*room);
        return true;
    }
    if (call == kClearHairOverrideGenericCall && me().isPerson()) {
        return true;
    }
    if (call == kRecalcWantsGenericCall) {
        if (thread_.temps[0] < 10) {
            if (!me().isPerson()) {
                throw Stop{Outcome::Error, "not a person", ""};
            }
        } else if (const auto* ids = world().neighbors(); ids == nullptr || !ids->contains(thread_.temps[1])) {
            throw Stop{Outcome::Error, "neighbor not found", std::to_string(thread_.temps[1])};
        }
        return true;
    }
    if (call == kAddToFamilyGenericCall) {
        return addToFamily();
    }
    if (call == kSetSkinGenericCall && (!me().isPerson() || thread_.temps[0] == 0)) {
        return me().isPerson();
    }
    throw Stop{Outcome::Unsupported, "Generic Sims Call not implemented", hex(call, 2)};
}

// "Add to family": the neighbor whose id is my stack object joins family temp 0, which is made
// when the neighborhood doesn't have it (32767 holds the NPCs). Its person data "family" (0x3D)
// and 0x43 are set, also on its Sim when on the lot. False when it is already in that family.
bool Execution::addToFamily() {
    constexpr std::size_t kFamilyField = 0x3D, kClearedOnJoin = 0x43;
    const std::int16_t neighborId = stackObject();
    const SavedNeighbor* neighbor = world().neighbor(neighborId);
    std::vector<std::int16_t>* data = neighbor ? world().neighborPersonData(neighborId) : nullptr;
    if (data == nullptr) {
        throw Stop{Outcome::Error, "missing neighbor for data access", std::to_string(neighborId)};
    }
    const std::int16_t family = thread_.temps[0];
    if (family == 0) {
        throw Stop{Outcome::Unsupported, "add to family: the lot's own household", ""};
    }
    const std::int16_t current = (*data)[kFamilyField];
    if (current == family) {
        return false;
    }
    if (current != 0) {
        throw Stop{Outcome::Unsupported, "add to family: moving out of another family", std::to_string(current)};
    }
    world().addFamily(static_cast<std::uint16_t>(family));
    (*data)[kFamilyField] = family;
    (*data)[kClearedOnJoin] = 0;
    const std::uint32_t guid = neighbor->guid;
    if (SimObject* sim = guid != 0 ? world().nextAfter(0, [&](const SimObject& s) { return s.isPerson() && s.definition.guid() == guid; })
                                   : nullptr) {
        sim->personData[kFamilyField] = family;
        sim->personData[kClearedOnJoin] = 0;
    }
    return true;
}

// Make New Character: a new neighbor of the age, gender and species in my locals (bytes 1, 2 and 14).
// Age and gender 0 or less are picked at random. Its id goes into the stack object's object
// array 0, which is emptied first. False when the neighborhood has no free id.
bool Execution::makeNewCharacter(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const std::uint8_t flags = o[9];
    SimObject* holder = world().find(stackObject());
    if (holder == nullptr) {
        throw Stop{Outcome::Error, "stack object does not exist", std::to_string(stackObject())};
    }
    if (holder->arrays.empty()) {
        throw Stop{Outcome::Error, "bad object array", "0"};
    }
    if (flags & kNewCharacterChild) {
        throw Stop{Outcome::Unsupported, "Make New Character for a child of two parents", ""};
    }
    if (flags & kNewCharacterTemplateFromToken) {
        throw Stop{Outcome::Unsupported, "Make New Character from an inventory token", ""};
    }
    // The template (else the species' own) gives the new character file, which isn't made yet.
    if (flags & kNewCharacterTemplateInTemps) {
        const std::uint32_t guid = static_cast<std::uint16_t>(thread_.temps[0]) |
                                   static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[1])) << 16;
        const ObjectType* type = code_.objectType(guid);
        if (type == nullptr) {
            throw Stop{Outcome::Error, "you sent me a crappy GUID, please fix it", hex(guid, 8)};
        }
        if (type->definition.field(objd::field::kType) == kNotCreatableObjectType) {
            throw Stop{Outcome::Error, "a new character can't be made from this object type", hex(guid, 8)};
        }
    }
    auto random = [&](std::uint32_t n) {
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(static_cast<std::uint32_t>(rng_())) * n) >> 32);
    };
    int age = slotAt(f.locals, o[1], "local");
    if (age <= 0) {
        age = 1 << random(6);
    }
    int gender = slotAt(f.locals, o[2], "local");
    gender = gender <= 0 ? (random(100) < 50 ? 2 : 1) : gender == 1 ? 1 : 2;
    const int species = slotAt(f.locals, o[14], "local");
    if (species < 0 || species > 3) {
        throw Stop{Outcome::Error, "species of a new character out of range", std::to_string(species)};
    }
    // The skin color (byte 0) and the outfit (flag 0x40) only shape the new character's look,
    // which isn't made yet.

    SavedNeighbor made;
    made.guid = 0;  // the new character's own GUID is not known yet
    made.personData.assign(kNeighborPersonDataCount, 0);
    made.personData[kSpeciesPersonData] = static_cast<std::int16_t>(species);
    made.personData[kNewAgePersonData] = newCharacterAge(age);
    if (age == kYoungAdultAgeFlag) {
        made.personData[kYoungAdultPersonData] = 1;
    }
    made.personData[kGenderPersonData] = gender == 1 ? 1 : 0;
    const std::optional<std::int16_t> id = world().addNeighbor(std::move(made));
    if (!id) {
        return false;
    }
    holder->arrays[0].assign(1, *id);
    return true;
}

// Person data "age" of a new character from its age flag: baby 0x20, toddler 1, child 2, teen 4,
// adult 8, young adult 0x40, anything else an elder.
std::int16_t Execution::newCharacterAge(int flag) {
    switch (flag) {
    case 0x20: return 1;
    case 0x01: return 2;
    case 0x02: return 3;
    case 0x04: return 16;
    case 0x08: return 19;
    case kYoungAdultAgeFlag: return 19;
    default: return 51;
    }
}

// Operands:
//   byte 0     variable
//   byte 1     flags: 0x01 fail if the variable doesn't exist, 0x02 neighbor ids, 0x04 write,
//              0x08 only A is an object (B, such as a neighbor id, just picks A's list)
//   bytes 2-4  object A (owner, data)
//   bytes 5-7  object B
//   bytes 8-10 value
// Missing variables are created unless flag 0x01 is set.
bool Execution::relationship(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const std::uint8_t variable = o[0];
    const std::uint8_t flags = o[1];
    const std::int16_t from = read(o[2], static_cast<std::uint16_t>(o[3] | o[4] << 8), f);
    const std::int16_t to = read(o[5], static_cast<std::uint16_t>(o[6] | o[7] << 8), f);
    // Neighbors keep 32-bit values: the primitive reads their low 16 bits and writes its value
    // sign-extended. Object lists keep the primitive's 16-bit values.
    std::vector<std::int16_t>* found = nullptr;
    std::vector<std::int32_t>* foundNeighbor = nullptr;
    if (flags & kRelationshipNeighborIds) {
        // A and B are neighbor ids: use A's list towards B.
        const auto& neighbors = world().neighbors();
        if (!neighbors) {
            throw Stop{Outcome::Unsupported, "Relationship with neighbor ids needs the neighborhood", ""};
        }
        if (!neighbors->contains(from) || !neighbors->contains(to)) {
            throw Stop{Outcome::Error, "neighbor not found", std::to_string(from) + " " + std::to_string(to)};
        }
        foundNeighbor = &world().neighborRelationship(from, to);
    } else if (flags & kRelationshipOnlyA) {
        if (world().find(from) == nullptr) {
            throw Stop{Outcome::Error, "could not find object reference", std::to_string(from)};
        }
        found = &world().relationship(from, to);
    } else {
        const SimObject* a = world().find(from);
        const SimObject* b = world().find(to);
        if (a == nullptr || b == nullptr) {
            throw Stop{Outcome::Error, "could not find object reference",
                       std::to_string(from) + " " + std::to_string(to)};
        }
        if (a->isPerson() && b->isPerson()) {
            // Between two Sims the variables live on their neighbors, found by GUID: A's list
            // towards B, the same one neighbor ids reach.
            if (!world().neighbors()) {
                throw Stop{Outcome::Unsupported, "Relationship between Sims needs the neighborhood", ""};
            }
            const std::int16_t neighborA = a->definition.guid() != 0 ? world().neighborIdOf(*a) : 0;
            const std::int16_t neighborB = b->definition.guid() != 0 ? world().neighborIdOf(*b) : 0;
            // The game doesn't check this and would crash. A Sim without a neighbor is a broken save.
            if (neighborA == 0 || neighborB == 0) {
                throw Stop{Outcome::Error, "Sim has no neighbor", std::to_string(from) + " " + std::to_string(to)};
            }
            foundNeighbor = &world().neighborRelationship(neighborA, neighborB);
        } else {
            found = &world().relationship(from, to);
        }
    }

    const auto access = [&](auto& values) {
        if (values.size() <= variable) {
            if (flags & kRelationshipFailIfMissing) {
                return false;
            }
            values.resize(variable + 1u, 0);
        }
        const auto valueData = static_cast<std::uint16_t>(o[9] | o[10] << 8);
        if (flags & kRelationshipWrite) {
            values[variable] = read(o[8], valueData, f);
        } else {
            write(o[8], valueData, f, static_cast<std::int16_t>(values[variable]));
        }
        return true;
    };
    return foundNeighbor != nullptr ? access(*foundNeighbor) : access(*found);
}

// Operands: byte 0 holds flags. Owner byte 9 / data 10-11 name the Sim. Byte 8 is the outfit category, or with
// flag 0x04 the value of owner byte 1 / data 2-3. The outfit's source GUID: flag 0x01 the stack
// object's type, 0x02 bytes 4-7, 0x40 temps 0 (low) and 1 (high). Flag 0x20 clears person data
// 0x77-0x78 (the source GUID). The Sim is dressed by its appearance. Scripts keep the category in
// person data 0x08 themselves. Always true.
bool Execution::changeOutfit(const bhav::Instruction& in, Frame& f) {
    constexpr std::uint8_t kFromStackObject = 0x01, kGuidInOperands = 0x02, kCategoryFromValue = 0x04,
                           kClearSource = 0x20, kGuidFromTemps = 0x40;
    constexpr std::size_t kSourceGuidLow = 0x77, kSourceGuidHigh = 0x78;
    const auto& o = in.operands;
    const std::uint8_t flags = o[0];
    const std::int16_t simId = read(o[9], static_cast<std::uint16_t>(o[10] | o[11] << 8), f);
    SimObject* sim = world().find(simId);
    if (sim == nullptr) {
        throw Stop{Outcome::Error, "invalid object id", std::to_string(simId)};
    }
    if (!sim->isPerson()) {
        throw Stop{Outcome::Error, "object referred to must be a person", std::to_string(simId)};
    }
    SimObject::OutfitChange change;
    if (flags & kFromStackObject) {
        if (const SimObject* stack = world().find(stackObject())) {
            change.sourceGuid = stack->definition.guid();
        }
    } else if (flags & kGuidInOperands) {
        change.sourceGuid = static_cast<std::uint32_t>(o[4] | o[5] << 8 | o[6] << 16) | static_cast<std::uint32_t>(o[7]) << 24;
    } else if (flags & kGuidFromTemps) {
        change.sourceGuid = static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[1])) << 16 |
                            static_cast<std::uint16_t>(thread_.temps[0]);
    }
    change.category = (flags & kCategoryFromValue) ? read(o[1], static_cast<std::uint16_t>(o[2] | o[3] << 8), f)
                                                   : static_cast<std::int16_t>(o[8]);
    if (flags & kClearSource) {
        sim->personData[kSourceGuidLow] = 0;
        sim->personData[kSourceGuidHigh] = 0;
    }
    sim->outfitChange = change;
    return true;
}

// Operands: bytes 4-5 whose (0 mine, 1 the stack object's), bytes 6-7 search type (0-2, which all
// sort the same motives). Temps 0-4 get the five lowest motives, lowest first. Motives less than
// 0.01 apart count as equal and keep their order.
bool Execution::findWorstMotives(const bhav::Instruction& in) {
    const auto& o = in.operands;
    const auto whose = static_cast<std::uint16_t>(o[4] | o[5] << 8);
    const auto type = static_cast<std::uint16_t>(o[6] | o[7] << 8);
    SimObject* sim = whose == 1 ? world().find(stackObject()) : whose == 2 ? nullptr : &me();
    if (sim == nullptr || !sim->isPerson()) {
        throw Stop{Outcome::Error, "Find 5 Worst Motives needs a person", std::to_string(whose)};
    }
    if (type > 2) {
        throw Stop{Outcome::Error, "search type is invalid", std::to_string(type)};
    }
    std::array<std::uint8_t, kWorstMotiveCandidates.size()> order = kWorstMotiveCandidates;
    for (std::size_t i = 1; i < order.size(); ++i) {
        for (std::size_t j = i; j > 0 && motive(*sim, order[j]) - motive(*sim, order[j - 1]) < -0.01f; --j) {
            std::swap(order[j], order[j - 1]);
        }
    }
    for (std::size_t i = 0; i < kWorstMotiveCount; ++i) {
        thread_.temps[i] = order[i];
    }
    return true;
}

// True when a tree on my stack runs on the stack object, i.e. I'm in an interaction with it.
bool Execution::testSimInteractingWith() {
    if (!me().isPerson()) {
        throw Stop{Outcome::Error, "primitive not supported for this type of object", hex(kTestSimInteractingWithOpcode, 4)};
    }
    const std::int16_t target = stackObject();
    return std::any_of(thread_.frames.begin(), thread_.frames.end(),
                       [target](const Frame& frame) { return frame.runningOnId == target; });
}

// Follow Sim does nothing in this game build: false for Sims, unsupported for other objects.
bool Execution::followSim() {
    if (!me().isPerson()) {
        throw Stop{Outcome::Error, "primitive not supported for this type of object", hex(kFollowSimOpcode, 4)};
    }
    return false;
}

// Set Motive Change (Sims only). Byte 3 bit 0 clears all of my motive changes. Otherwise it adds a
// change for motive byte 2: a rate per Sim hour (owner byte 0, data bytes 4-5) up to a limit
// (owner byte 1, data bytes 6-7). With byte 3 bit 1 the motive also stops decaying by itself:
// its daily rate (person data) is set to 0.
bool Execution::setMotiveChange(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    SimObject& sim = me();
    if (!sim.isPerson()) {
        throw Stop{Outcome::Unsupported, "Set Motive Change on an object that is not a person", ""};
    }
    if (o[3] & 0x01) {
        sim.motiveChanges.clear();
        return true;
    }
    const std::uint8_t motive = o[2];
    constexpr std::array<std::uint8_t, 9> kChangeableMotives{0, 1, 5, 6, 7, 8, 9, 14, 15};
    if (std::find(kChangeableMotives.begin(), kChangeableMotives.end(), motive) == kChangeableMotives.end()) {
        throw Stop{Outcome::Error, "motive number out of range", std::to_string(motive)};
    }
    const std::int16_t rate = read(o[0], static_cast<std::uint16_t>(o[4] | o[5] << 8), f);
    const std::int16_t limit = read(o[1], static_cast<std::uint16_t>(o[6] | o[7] << 8), f);
    // A limit of ours, so a script adding changes every tick can't grow the list without end.
    constexpr std::size_t kMaxMotiveChanges = 64;
    if (sim.motiveChanges.size() >= kMaxMotiveChanges) {
        diag::warnOnce("motives: more than 64 motive changes at once drops the oldest");
        sim.motiveChanges.erase(sim.motiveChanges.begin());
    }
    if (o[3] & 0x02) {
        sim.personData[motiveRateField(motive)] = 0;
    }
    sim.motiveChanges.push_back({motive, rate / kTicksPerSimHour, static_cast<float>(limit)});
    return true;
}

// Want Satisfy: satisfies the Sim's current wants that match. Operands:
//   byte 0, 1-2  the Sim (owner, data): an object id, or a neighbor id with flag bit 0
//   bytes 3-6    want GUID
//   byte 7, 8-9  argument (owner, data), what it is depends on the want
//   byte 10, 11-12 value (owner, data)
//   byte 13      flags: bit 0 the Sim is a neighbor id, bit 1 a Sim argument is a neighbor id
// True when at least one want matched. Matched wants leave the Sim's list. The aspiration
// points and memories they give are not implemented.
bool Execution::wantSatisfy(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    if (!world().neighbors() || !world().hasWantDefinitions()) {
        throw Stop{Outcome::Unsupported, "Want Satisfy needs the neighborhood and the want definitions", ""};
    }
    const std::uint8_t flags = o[13];
    const std::int16_t sim = toNeighbor(read(o[0], static_cast<std::uint16_t>(o[1] | o[2] << 8), f), flags & 0x01);
    const std::uint32_t guid = static_cast<std::uint32_t>(o[3] | o[4] << 8 | o[5] << 16) | static_cast<std::uint32_t>(o[6]) << 24;
    const content::WantDefinition* def = world().wantDefinition(guid);
    if (def == nullptr) {
        throw Stop{Outcome::Error, "data number out of range", hex(guid, 8)};
    }
    const std::int16_t argument = read(o[7], static_cast<std::uint16_t>(o[8] | o[9] << 8), f);
    const std::int16_t value = read(o[10], static_cast<std::uint16_t>(o[11] | o[12] << 8), f);
    if (!def->simArrayTree.empty()) {
        throw Stop{Outcome::Unsupported, "Want Satisfy for wants with a Sim list tree", def->simArrayTree};
    }

    // The request the Sim's wants are compared with.
    swaf::Want request;
    request.id = static_cast<std::uint16_t>(sim);
    request.guid = guid;
    request.type = static_cast<std::uint8_t>(def->argument);
    request.value = value;
    switch (def->argument) {
    case content::WantArgument::None:
        request.value = argument;
        break;
    case content::WantArgument::Sim:
        request.argument = toNeighbor(argument, flags & 0x02);
        break;
    case content::WantArgument::Object:
    case content::WantArgument::Category:
        // Category wants are satisfied with an object. Its type decides the category.
        request.type = static_cast<std::uint8_t>(content::WantArgument::Object);
        request.argument = argument == 0 ? 0 : static_cast<std::int32_t>(objectGuid(argument));
        break;
    case content::WantArgument::Skill:
        request.argument = argument;
        break;
    case content::WantArgument::Career:
    case content::WantArgument::Badge:
        request.argument = static_cast<std::int32_t>(objectGuid(argument));
        break;
    }

    auto& wants = world().neighborWants(sim);
    const auto matched = std::remove_if(wants.begin(), wants.end(), [&](const swaf::Want& w) {
        return wantMatches(w, request, def->op);
    });
    const bool satisfied = matched != wants.end();
    wants.erase(matched, wants.end());
    return satisfied;
}

// A Sim reference as a neighbor id: an object id turned into its Sim's neighbor id, or a
// neighbor id checked to exist. Not a Sim (or an unknown neighbor) is an error.
std::int16_t Execution::toNeighbor(std::int16_t id, bool isNeighborId) {
    std::int16_t neighbor = 0;
    if (isNeighborId) {
        neighbor = world().neighbors()->contains(id) ? id : 0;
    } else {
        const SimObject* object = world().find(id);
        if (object == nullptr) {
            throw Stop{Outcome::Error, "could not find object reference", std::to_string(id)};
        }
        neighbor = world().neighborIdOf(*object);
    }
    if (neighbor == 0) {
        throw Stop{Outcome::Error, "object referred to must be a person", std::to_string(id)};
    }
    return neighbor;
}

std::uint32_t Execution::objectGuid(std::int16_t id) {
    const SimObject* object = world().find(id);
    if (object == nullptr) {
        throw Stop{Outcome::Error, "could not find object reference", std::to_string(id)};
    }
    return object->definition.guid();
}

// Whether a Sim's want is satisfied by the request. An argument of 0 in the want accepts any.
bool Execution::wantMatches(const swaf::Want& want, const swaf::Want& request, content::WantOperator op) const {
    if (want.id != request.id || want.guid != request.guid) {
        return false;
    }
    auto compare = [op](std::int32_t got, std::int32_t wanted) {
        switch (op) {
        case content::WantOperator::Equals:              return got == wanted;
        case content::WantOperator::LessThan:            return got < wanted;
        case content::WantOperator::GreaterThan:         return got > wanted;
        case content::WantOperator::LessThanOrEquals:    return got <= wanted;
        case content::WantOperator::GreaterThanOrEquals: return got >= wanted;
        case content::WantOperator::None:                return true;
        }
        return true;
    };
    using content::WantArgument;
    switch (static_cast<WantArgument>(want.type)) {
    case WantArgument::None:
        break;
    case WantArgument::Sim:
        if (want.argument != 0 && want.argument != request.argument) {
            return false;
        }
        break;
    case WantArgument::Object:
        if (want.argument != 0 && request.argument != 0 && want.argument != request.argument) {
            return false;
        }
        break;
    case WantArgument::Category:
        if (want.argument != 0) {
            if (request.type == static_cast<std::uint8_t>(WantArgument::Object)) {
                const ObjectType* type = code_.objectType(static_cast<std::uint32_t>(request.argument));
                if (type == nullptr || type->definition.field(kWantCategoryField) != want.argument) {
                    return false;
                }
            } else if (request.type == static_cast<std::uint8_t>(WantArgument::Category)) {
                return want.argument == request.argument;
            }
        }
        break;
    case WantArgument::Skill:
    case WantArgument::Career:
    case WantArgument::Badge: {
        const bool argumentOk = want.argument == 0 || want.argument == request.argument;
        return want.value == 0 ? argumentOk : argumentOk && want.value == request.value;
    }
    default:
        return false;
    }
    return compare(request.value, want.value);
}

} // namespace ts2::sim::vm_detail
