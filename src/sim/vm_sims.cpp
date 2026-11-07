#include "sim/vm_execution.hpp"
#include "sim/want_refill.hpp"

#include "formats/propset.hpp"

#include <limits>

namespace ts2::sim::vm_detail {

namespace {

// The keys of the part lists in a Sim's age data that hold a part ("listcntStd" with "lks"/"lss",
// or without it the older "listcnt" with "lk"/"ls"). Each list is keyed (category << 16) | part.
std::set<std::uint32_t> filledOutfitLists(const std::map<std::string, std::string>& age) {
    const bool newer = age.contains("listcntStd");
    const std::string count = newer ? "listcntStd" : "listcnt", key = newer ? "lks" : "lk", size = newer ? "lss" : "ls";
    // A broken file could claim billions of lists: there can't be more than it has properties.
    const std::uint32_t lists = std::min<std::uint32_t>(propset::number(age, count), static_cast<std::uint32_t>(age.size()));
    std::set<std::uint32_t> filled;
    for (std::uint32_t i = 0; i < lists; ++i) {
        if (propset::number(age, size + std::to_string(i)) != 0) {
            filled.insert(propset::number(age, key + std::to_string(i)));
        }
    }
    return filled;
}

// Whether a Sim has clothes for an outfit category: a top and a bottom, or a full body.
bool hasClothesFor(const std::set<std::uint32_t>& filled, std::uint32_t category) {
    constexpr std::uint32_t kTop = 0x04, kBody = 0x08, kBottom = 0x10;
    const auto has = [&](std::uint32_t part) { return filled.contains(category << 16 | part); };
    return (has(kTop) && has(kBottom)) || has(kBody);
}

} // namespace

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
    if (call == kDisableBuildBuyGenericCall) {
        world().blockBuildBuy(me().id);
        return true;
    }
    if (call == kEnableBuildBuyGenericCall) {
        world().unblockBuildBuy(me().id);
        return true;
    }
    if (call == kAbortInteractionsGenericCall) {
        // Objects queued for delete are still found.
        if (world().findIncludingRemoved(stackObject()) == nullptr) {
            throw Stop{Outcome::Error, "stack object does not exist", std::to_string(stackObject())};
        }
        tellSimsObjectGoes(stackObject());
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
        const std::int16_t mode = thread_.temps[0];
        std::int16_t neighbor = 0, value = 0;
        if (mode < 10) {
            if (!me().isPerson()) {
                throw Stop{Outcome::Error, "not a person", ""};
            }
            neighbor = world().neighborIdOf(me());
            value = thread_.temps[1];
        } else if (const auto* ids = world().neighbors(); ids == nullptr || !ids->contains(thread_.temps[1])) {
            throw Stop{Outcome::Error, "neighbor not found", std::to_string(thread_.temps[1])};
        } else {
            neighbor = thread_.temps[1];
            value = thread_.temps[2];
        }
        // Modes 0 and 10 queue the Sim for new wants, the value (0..3, else 0) saying how much goes.
        // Mode 10 leaves a neighbor alone when none of its Sims is on the lot. The other modes are
        // not done.
        const bool onLot = mode < 10 || world().nextAfter(0, [&](const SimObject& o) { return o.isPerson() && world().neighborIdOf(o) == neighbor; }) != nullptr;
        if ((mode == 0 || mode == 10) && onLot) {
            queueWantRefill(world(), neighbor, value >= 0 && value <= 3 ? value : 0);
        }
        return true;
    }
    if (call == kAddChildGenericCall) {
        const auto* ids = world().neighbors();
        for (const std::int16_t id : {thread_.temps[0], thread_.temps[1]}) {
            if (ids == nullptr || !ids->contains(id)) {
                throw Stop{Outcome::Error, "neighbor not found", std::to_string(id)};
            }
        }
        world().setFamilyTie(thread_.temps[0], thread_.temps[1], famt::TieType::Child);
        return true;
    }
    if (call == kRemoveFamilyTieGenericCall) {
        const auto* ids = world().neighbors();
        for (const std::int16_t id : {thread_.temps[0], thread_.temps[1]}) {
            if (ids == nullptr || !ids->contains(id)) {
                throw Stop{Outcome::Error, "neighbor not found", std::to_string(id)};
            }
        }
        world().removeFamilyTie(thread_.temps[0], thread_.temps[1]);
        return true;
    }
    if (call == kRemappedNeighborIdGenericCall) {
        if (world().neighborRemapUnread()) {
            throw Stop{Outcome::Unsupported, "Get Remapped Neighbor Id with unreadable remap tables", ""};
        }
        // The id is sign-extended, so a negative one is never found.
        const auto id = static_cast<std::uint32_t>(static_cast<std::int32_t>(thread_.temps[0]));
        std::int16_t result = -1;
        if (const auto& remap = world().neighborRemap()) {
            if (const auto it = remap->find(id); it != remap->end()) {
                result = static_cast<std::int16_t>(it->second);
            }
        }
        thread_.temps[1] = result;
        return true;
    }
    if (call == kFitsWithFootprintGenericCall) {
        return fitsWithFootprint();
    }
    if (call == kOutfitsInformationGenericCall) {
        return outfitsInformation();
    }
    if (call == kAddToFamilyGenericCall) {
        return addToFamily();
    }
    if (call == kUnlinkCharacterGenericCall) {
        return unlinkCharacter();
    }
    if (call == kSetSkinGenericCall && (!me().isPerson() || thread_.temps[0] == 0)) {
        return me().isPerson();
    }
    throw Stop{Outcome::Unsupported, "Generic Sims Call not implemented", hex(call, 2)};
}

// Generic Sims Call 0x26: the neighbor in temp 0 is unlinked from its character when its Sim has a
// character file of its own and isn't unlinked yet (person data 0xA5). Every Sim of its type on the
// lot is deleted, then World::unlinkNeighbor does the rest. The game also removes the character
// file's resources, which waits for saving, and sends message 0x9E, whose readers are not known
// yet. Always true, for an unknown neighbor too.
bool Execution::unlinkCharacter() {
    constexpr std::size_t kUnlinkedPersonData = 0xA5;
    const std::int16_t id = thread_.temps[0];
    const SavedNeighbor* neighbor = id >= 1 ? world().neighbor(id) : nullptr;
    const auto& hasCharacterFile = world().hasCharacterFile();
    if (neighbor == nullptr || !hasCharacterFile || !hasCharacterFile(id) || world().neighborPersonValue(id, kUnlinkedPersonData) != 0) {
        return true;
    }
    const std::uint32_t guid = neighbor->guid;
    const auto ofType = [guid](const SimObject& o) { return o.definition.guid() == guid; };
    while (SimObject* sim = world().nextAfter(std::numeric_limits<std::int16_t>::min(), ofType)) {
        takeOutOfWorld(*sim);
        world().remove(*sim);
    }
    world().unlinkNeighbor(id);
    return true;
}

// The outfit category number in temp 0 becomes the category's flag (0 and any number not listed
// is everyday). Temp 3 = 0 asks about the family's wardrobe and -1 about every character, which
// are not done yet. A missing object, or one without looks, has none.
bool Execution::outfitsInformation() {
    constexpr std::array<std::uint32_t, 14> kCategoryFlags{1, 0x80, 8, 0x200, 0x20, 0x10, 0x400, 1, 0x100, 2, 4, 0x40, 1, 0x1000};
    const std::int16_t who = thread_.temps[3];
    if (who == 0 || who == -1) {
        throw Stop{Outcome::Unsupported, "Get Outfits Information for the family or every character not implemented", std::to_string(who)};
    }
    const auto number = static_cast<std::size_t>(static_cast<std::uint16_t>(thread_.temps[0]));
    const std::uint32_t category = number < kCategoryFlags.size() ? kCategoryFlags[number] : 1;
    std::int16_t result = 0;
    if (const SimObject* object = world().findIncludingRemoved(who)) {
        const auto& hasOutfit = world().hasOutfit();
        if (!hasOutfit) {
            throw Stop{Outcome::Unsupported, "Get Outfits Information needs the Sims' looks", ""};
        }
        result = hasOutfit(*object, category).value_or(false) ? 1 : 0;
    }
    thread_.temps[1] = result;
    return true;
}

// The game swaps the footprint index in, runs the test where the object stands and swaps it back.
bool Execution::fitsWithFootprint() {
    SimObject* object = world().find(stackObject());
    if (object == nullptr) {
        throw Stop{Outcome::Error, "stack object does not exist", std::to_string(stackObject())};
    }
    const std::int16_t footprint = thread_.temps[0];
    if (object->data[kFootprintIndexField] == footprint) {
        return true;
    }
    const auto& fits = world().objectFits();
    if (!fits) {
        return true;
    }
    if (object->x == kOffLot || object->y == kOffLot) {
        throw Stop{Outcome::Unsupported, "footprint test for an object off the lot", std::to_string(object->id)};
    }
    const std::int16_t saved = object->data[kFootprintIndexField];
    object->data[kFootprintIndexField] = footprint;
    bool result = false;
    try {
        result = fits(*object, static_cast<float>(object->x) / 65536.0f, static_cast<float>(object->y) / 65536.0f, object->level,
                      object->data[kDirectionField]);
    } catch (...) {
        object->data[kFootprintIndexField] = saved;
        throw;
    }
    object->data[kFootprintIndexField] = saved;
    return result;
}

// "Add to family": the neighbor whose id is my stack object joins family temp 0, or with 0 the
// household living on the lot, which is made when the neighborhood doesn't have it (32767 holds the
// NPCs). Its person data "family" (0x3D) and 0x43 are set, also on its Sim when on the lot, its
// GUID joins the family's members, and temp 0 becomes 0. False when it is already in that family. A lot with no household makes the
// game wait for one, which isn't done.
bool Execution::addToFamily() {
    constexpr std::size_t kFamilyField = 0x3D, kClearedOnJoin = 0x43;
    const std::int16_t neighborId = stackObject();
    const SavedNeighbor* neighbor = world().neighbor(neighborId);
    std::vector<std::int16_t>* data = neighbor ? world().neighborPersonData(neighborId) : nullptr;
    if (data == nullptr) {
        throw Stop{Outcome::Error, "missing neighbor for data access", std::to_string(neighborId)};
    }
    std::int16_t family = thread_.temps[0];
    if (family == 0) {
        const std::optional<std::uint32_t> household = world().lotHousehold();
        if (!household) {
            throw Stop{Outcome::Unsupported, "add to family: no household on the lot", ""};
        }
        family = static_cast<std::int16_t>(*household);
    }
    const std::int16_t current = (*data)[kFamilyField];
    if (current == family) {
        return false;
    }
    if (current != 0) {
        throw Stop{Outcome::Unsupported, "add to family: moving out of another family", std::to_string(current)};
    }
    world().addFamily(static_cast<std::uint16_t>(family));
    thread_.temps[0] = 0;
    (*data)[kFamilyField] = family;
    (*data)[kClearedOnJoin] = 0;
    const std::uint32_t guid = neighbor->guid;
    world().addFamilyMember(static_cast<std::uint16_t>(family), guid);
    if (SimObject* sim = guid != 0 ? world().nextAfter(0, [&](const SimObject& s) { return s.isPerson() && s.definition.guid() == guid; })
                                   : nullptr) {
        sim->personData[kFamilyField] = family;
        sim->personData[kClearedOnJoin] = 0;
    }
    return true;
}

// Make New Character: a new neighbor of the age, gender and species in my locals (bytes 1, 2 and 14).
// Age and gender 0 or less are picked at random. Its Sim gets a definition of its own (see
// madeCharacterType). Its id goes into the stack object's object array 0, which is emptied first.
// False when the neighborhood has no free id or the template type is missing. Flag 0x01 makes
// babies of two parents instead (see makeChildren).
std::optional<bool> Execution::makeNewCharacter(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const std::uint8_t flags = o[9];
    SimObject* holder = world().find(stackObject());
    if (holder == nullptr) {
        throw Stop{Outcome::Error, "stack object does not exist", std::to_string(stackObject())};
    }
    if (holder->arrays.empty()) {
        throw Stop{Outcome::Error, "bad object array", "0"};
    }
    if (flags & kNewCharacterTemplateFromToken) {
        throw Stop{Outcome::Unsupported, "Make New Character from an inventory token", ""};
    }
    // The template the new Sim's definition is made from: the type in temps 0 and 1, else the species' own.
    std::optional<std::uint32_t> templateGuid;
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
        templateGuid = guid;
    }
    if (flags & kNewCharacterChild) {
        return makeChildren(in, f, *holder, templateGuid);
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
    // The skin (byte 0) and the outfit object only shape the new character's look, which the lot's
    // drawing builds.
    const int skin = slotAt(f.locals, o[0], "local");
    std::uint32_t outfit = 0;
    if (flags & kNewCharacterOutfit) {
        outfit = (flags & kNewCharacterOutfitInTemps)
                     ? static_cast<std::uint16_t>(thread_.temps[2]) | static_cast<std::uint32_t>(static_cast<std::uint16_t>(thread_.temps[3])) << 16
                     : static_cast<std::uint32_t>(o[10] | o[11] << 8 | o[12] << 16 | o[13] << 24);
    }

    // Human, large dog, small dog and cat templates.
    constexpr std::array<std::uint32_t, 4> kSpeciesTemplates{0x7FD96B54, 0x0F83C946, 0x111D8FFF, 0x911D900A};
    const ObjectType* type = code_.objectType(templateGuid.value_or(kSpeciesTemplates[static_cast<std::size_t>(species)]));
    // Without a neighborhood there is no neighbor to add, so nothing is made. Otherwise each retry
    // would keep a new definition.
    if (type == nullptr || world().neighbors() == nullptr) {
        return false;
    }
    SavedNeighbor made;
    made.guid = madeCharacterType(*type, random(8));
    made.personData.assign(kNeighborPersonDataCount, 0);
    made.personData[kSpeciesPersonData] = static_cast<std::int16_t>(species);
    made.personData[kNewAgePersonData] = newCharacterAge(age);
    if (age == kYoungAdultAgeFlag) {
        made.personData[kYoungAdultPersonData] = 1;
    }
    made.personData[kGenderPersonData] = gender == 1 ? 1 : 0;
    made.madeAs = SavedNeighbor::MadeAs{age, gender, skin, outfit};
    const std::optional<std::int16_t> id = world().addNeighbor(std::move(made));
    if (!id) {
        return false;
    }
    nameNewCharacter(*id, *type, species, gender);
    holder->arrays[0].assign(1, *id);
    return true;
}

// A Sim Make New Character makes from a template gets a first name and a surname drawn from the
// game's name tables by species and gender, first name first, each drawn among the table's entries
// by the character factory's generator. A table missing draws nothing. A template whose definition
// field 0x30 is set keeps its own first name and gets no surname.
void Execution::nameNewCharacter(std::int16_t id, const ObjectType& type, int species, int gender) {
    constexpr std::size_t kKeepsName = 0x30;
    if (type.definition.field(kKeepsName) != 0) {
        world().setNeighborSurname(id, "");
        return;
    }
    const bool female = gender == 1;
    const bool human = species == 0;
    const std::uint16_t firstNames = human ? (female ? 0x97 : 0x96) : (female ? 0xEB : 0xEA);
    const std::uint16_t surnames = human ? (female ? 0xCC : 0x98) : (female ? 0xED : 0xEC);
    const auto draw = [&](std::uint16_t table) -> std::optional<std::string> {
        const auto* names = world().nameTable(table);
        if (names == nullptr || names->empty()) {
            return std::nullopt;
        }
        const auto pick = (static_cast<std::uint64_t>(world().characterRandom()()) * names->size()) >> 32;
        return (*names)[static_cast<std::size_t>(pick)];
    };
    if (auto first = draw(firstNames)) {
        world().setNeighborFirstName(id, std::move(*first));
    }
    if (auto last = draw(surnames)) {
        world().setNeighborSurname(id, std::move(*last));
    }
}

// Make New Character for babies of two parents (flag 0x01). Parent 1 is bytes 6, 7-8 and parent 2
// bytes 3, 4-5 (owner, data), each a neighbor id with flag 0x02 or 0x04, else an object whose Sim
// names its neighbor. Temp 0 says how many babies, 1 to 10. The parents must be one species, though
// a large and a small dog may mix. The babies' neighbor ids go into the stack object's object array 0.
// The instruction then waits for them to be built and is true. How many ticks the game's build takes
// is not known yet, it waits one. The game also posts a message (0xAD90F890) unless byte 15 bit 0 is
// set, which nothing receives yet.
std::optional<bool> Execution::makeChildren(const bhav::Instruction& in, Frame& f, SimObject& holder,
                                            std::optional<std::uint32_t> templateGuid) {
    constexpr std::int16_t kMaxChildren = 10;
    constexpr std::int16_t kSpeciesFlagDogs = 0x06;
    if (f.childrenPending) {
        f.childrenPending = false;
        return true;
    }
    const auto& o = in.operands;
    holder.arrays[0].clear();
    // A parent's neighbor: by id, or the neighbor whose Sim the object is (0 for a missing object).
    auto parent = [&](std::uint8_t owner, std::uint16_t data, std::uint8_t neighborFlag) -> std::int16_t {
        const std::int16_t value = read(owner, data, f);
        if (o[9] & neighborFlag) {
            if (world().neighbor(value) == nullptr) {
                throw Stop{Outcome::Error, "neighbor not found", std::to_string(value)};
            }
            return value;
        }
        const SimObject* object = world().find(value);
        if (object == nullptr) {
            return 0;
        }
        const std::int16_t id = object->definition.guid() != 0 ? world().neighborIdWithGuid(object->definition.guid()) : 0;
        if (id == 0) {
            throw Stop{Outcome::Error, "neighbor not found", std::to_string(value)};
        }
        return id;
    };
    const std::int16_t parent1 = parent(o[6], static_cast<std::uint16_t>(o[7] | o[8] << 8), kNewCharacterParent1Neighbor);
    const std::int16_t parent2 = parent(o[3], static_cast<std::uint16_t>(o[4] | o[5] << 8), kNewCharacterParent2Neighbor);
    const std::int16_t count = thread_.temps[0];
    if (count < 1 || count > kMaxChildren) {
        throw Stop{Outcome::Error, "number of new characters out of range", std::to_string(count)};
    }
    if (parent1 == 0 || parent2 == 0) {
        throw Stop{Outcome::Error, "neighbor not found", "0"};
    }
    const std::int16_t species1 = speciesFlag(parent1), species2 = speciesFlag(parent2);
    if (species1 != species2 && ((species1 & kSpeciesFlagDogs) == 0 || (species2 & kSpeciesFlagDogs) == 0)) {
        throw Stop{Outcome::Error, "parents of different species", std::to_string(species1) + " " + std::to_string(species2)};
    }
    for (std::int16_t i = 0; i < count; ++i) {
        if (const std::optional<std::int16_t> id = makeChild(parent1, parent2, templateGuid)) {
            holder.arrays[0].push_back(*id);
        }
    }
    f.childrenPending = true;
    return std::nullopt;
}

// Generic Sims Call 0x1D: true once the character of the neighbor in temp 0 is loaded, or when there
// is none. Until then it starts the load and waits a tick at a time, for up to temp 1 ticks (no end
// below 0), and is true when they run out. Loads take as long as they take, so Sims made one after
// the other appear one after the other, as the game's cars do. The game skips the wait in some state
// that isn't known yet.
std::optional<bool> Execution::loadCharacter(Frame& f) {
    if (!f.characterWaits) {
        f.characterWaits = thread_.temps[1];
    }
    const auto& load = world().characterLoad();
    const std::int16_t neighbor = thread_.temps[0];
    if (!load || world().neighbor(neighbor) == nullptr) {
        return true;
    }
    if (const std::optional<bool> loaded = load(neighbor); !loaded || *loaded) {
        return true;
    }
    std::int16_t& left = *f.characterWaits;
    if (left == 0) {
        return true;
    }
    if (left > 0) {
        --left;
    }
    return std::nullopt;
}

// A neighbor's species as the game's flag: 1 human, 2 large dog, 4 small dog, 8 cat (person data
// 0xBA 1-3, anything else counts as human).
std::int16_t Execution::speciesFlag(std::int16_t neighbor) {
    const std::int16_t species = world().neighborPersonValue(neighbor, kSpeciesPersonData);
    return species == 1 ? 2 : species == 2 ? 4 : species == 3 ? 8 : 1;
}

// One baby of two neighbors as the game's factory makes it, or nothing when it can't. Without a
// template it takes the species of a parent picked at random, and that species' template. With a
// template given it is human. The baby is 1 (a baby) with skin data 0x52 = 500 and 0x51 = 0. When
// exactly one parent has person data 0xA5 set, the baby's gender is the other parent's opposite.
// Otherwise it is drawn 50/50. It is named "Baby Boy" or "Baby Girl" until the player names it,
// and takes the first parent's surname (a birth gives the one who carried it first). A
// human baby inherits its genes (see childGenome) from both parents, the first given first. When
// exactly one parent has person data 0xA5 set, the other stands for both. Its look is not made yet.
std::optional<std::int16_t> Execution::makeChild(std::int16_t parent1, std::int16_t parent2,
                                                 std::optional<std::uint32_t> templateGuid) {
    constexpr std::size_t kMarkPersonData = 0xA5, kSkinPersonData = 0x51, kSkinShadePersonData = 0x52;
    constexpr std::int16_t kBabyAge = 1, kBabySkinShade = 500;
    constexpr std::array<std::uint32_t, 4> kSpeciesTemplates{0x7FD96B54, 0x0F83C946, 0x111D8FFF, 0x911D900A};
    auto random = [&](std::uint32_t n) {
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(static_cast<std::uint32_t>(rng_())) * n) >> 32);
    };
    if (world().neighbor(parent1) == nullptr || world().neighbor(parent2) == nullptr || world().neighbors() == nullptr) {
        return std::nullopt;
    }
    std::int16_t flag = 1;
    if (!templateGuid) {
        flag = speciesFlag(random(100) > 50 ? parent1 : parent2);
        templateGuid = kSpeciesTemplates[flag == 2 ? 1 : flag == 4 ? 2 : flag == 8 ? 3 : 0];
    }
    const ObjectType* type = code_.objectType(*templateGuid);
    if (type == nullptr) {
        return std::nullopt;
    }
    SavedNeighbor made;
    made.guid = madeCharacterType(*type, random(8));
    made.personData.assign(kNeighborPersonDataCount, 0);
    // A draw at or below one half is a girl, unless one parent decides.
    const bool drawnFemale = static_cast<double>(static_cast<std::uint32_t>(rng_())) / 4294967296.0 <= 0.5;
    std::optional<std::int16_t> decides = parent1 == parent2 ? std::optional<std::int16_t>{parent1} : std::nullopt;
    const bool marked1 = world().neighborPersonValue(parent1, kMarkPersonData) != 0;
    const bool marked2 = world().neighborPersonValue(parent2, kMarkPersonData) != 0;
    if (marked1 != marked2) {
        decides = marked1 ? parent2 : parent1;
    }
    const bool female = decides ? world().neighborPersonValue(*decides, kGenderPersonData) != 1 : drawnFemale;
    made.personData[kNewAgePersonData] = kBabyAge;
    made.personData[kSpeciesPersonData] = static_cast<std::int16_t>(flag == 2 ? 1 : flag == 4 ? 2 : flag == 8 ? 3 : 0);
    made.personData[kGenderPersonData] = female ? 1 : 0;
    made.personData[kSkinShadePersonData] = kBabySkinShade;
    made.personData[kSkinPersonData] = 0;
    if (flag == 1) {
        const std::int16_t geneticA = marked1 != marked2 ? *decides : parent1;
        const std::int16_t geneticB = marked1 != marked2 ? *decides : parent2;
        const std::optional<Genome> a = parentGenes(geneticA), b = parentGenes(geneticB);
        if (const auto* catalog = world().geneticsCatalog(); catalog != nullptr && a && b) {
            made.genes = childGenome(*a, *b, *catalog, world().geneticsRandom(), &made.facePicks);
            made.faceParentA = geneticA;
            made.faceParentB = geneticB;
            // The eyebrows are drawn after the genes, from the catalog when the look is made. The
            // game draws them with the birth job's generator, not the genes', so they are drawn with
            // the factory's other draws here and the genes' stream stays as the game's.
            made.browDraw = static_cast<std::uint32_t>(rng_());
        }
    }
    const std::string surname = neighborSurname(parent1);
    const std::optional<std::int16_t> id = world().addNeighbor(std::move(made));
    if (id && !world().babyFirstName(female).empty()) {
        world().setNeighborFirstName(*id, world().babyFirstName(female));
    }
    if (id) {
        world().setNeighborSurname(*id, surname);
    }
    return id;
}

std::string Execution::neighborSurname(std::int16_t neighbor) {
    constexpr std::size_t kSurnameEntry = 2;
    if (auto taken = world().neighborSurname(neighbor)) {
        return std::move(*taken);
    }
    const SavedNeighbor* saved = world().neighbor(neighbor);
    const ObjectType* type = saved ? code_.objectType(saved->guid) : nullptr;
    if (type == nullptr) {
        return {};
    }
    const auto set = static_cast<std::uint16_t>(type->definition.field(objd::field::kCatalogStringsId));
    return code_.catalogString(type->group, set, kSurnameEntry).value_or("");
}

// A parent's genes: its saved DNA, else made from its age data as the game makes them (both
// alleles of skin, hair and eyes its own colour). None without either.
std::optional<Genome> Execution::parentGenes(std::int16_t neighbor) {
    const SavedNeighbor* saved = world().neighbor(neighbor);
    if (saved == nullptr) {
        return std::nullopt;
    }
    if (!saved->genes.empty()) {
        return saved->genes;
    }
    const ObjectType* type = saved->guid != 0 ? code_.objectType(saved->guid) : nullptr;
    const auto* age = type ? code_.ageData(type->group) : nullptr;
    if (age == nullptr) {
        return std::nullopt;
    }
    const auto colour = [&](const char* key) {
        const auto it = age->find(key);
        return it != age->end() ? it->second : std::string{};
    };
    const Gene skin{colour("skincolor"), colour("skincolor")};
    return Genome{{kHairGene, Gene{colour("haircolor"), colour("haircolor")}},
                  {kSkinAllelesGene, skin},
                  {kEyeGene, Gene{colour("eyecolor"), colour("eyecolor")}},
                  {kSkinGene, skin}};
}

// The definition the game gives a new character: its template's, now a person (type 2), under a
// new GUID that also fills fields 0x1D and 0x1E, with the template's GUID in fields 0x46 and 0x47.
// A new GUID is `top` (0-7) in its top three bits over a 29-bit count. Returns that GUID.
// Field 0x2C also changes in the game (to a value the same for all of one game's new Sims), what
// it holds is not known yet and it keeps the template's.
std::uint32_t Execution::madeCharacterType(const ObjectType& from, std::uint32_t top) {
    constexpr std::size_t kGuidCopy1 = 0x1D, kGuidCopy2 = 0x1E, kTemplateGuid1 = 0x46, kTemplateGuid2 = 0x47;
    std::uint32_t guid = 0;
    // A GUID some type already has would hide it, so such a count is skipped (an engine guard, the
    // game's time count doesn't meet saved GUIDs).
    do {
        guid = (top & 7u) << 29 | world().nextGuidCount();
    } while (code_.objectType(guid) != nullptr);
    ObjectType made = from;
    auto& fields = made.definition.fields;
    fields.resize(std::max<std::size_t>(fields.size(), kTemplateGuid2 + 1), 0);
    const std::uint32_t templateGuid = from.definition.guid();
    fields[objd::field::kType] = kPersonType;
    fields[objd::field::kGuid1] = fields[kGuidCopy1] = static_cast<std::uint16_t>(guid);
    fields[objd::field::kGuid2] = fields[kGuidCopy2] = static_cast<std::uint16_t>(guid >> 16);
    fields[kTemplateGuid1] = static_cast<std::uint16_t>(templateGuid);
    fields[kTemplateGuid2] = static_cast<std::uint16_t>(templateGuid >> 16);
    if (code_.addMadeObjectType(std::move(made)) == nullptr) {
        throw Stop{Outcome::Unsupported, "this code source can't hold a new character's definition", ""};
    }
    return guid;
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
        if (world().findIncludingRemoved(from) == nullptr) {
            throw Stop{Outcome::Error, "could not find object reference", std::to_string(from)};
        }
        found = &world().relationship(from, to);
    } else {
        // An object removed this tick is still found by id, as in the game.
        const SimObject* a = world().findIncludingRemoved(from);
        const SimObject* b = world().findIncludingRemoved(to);
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
// 0x77-0x78 (the source GUID). A Sim with age data is dressed from its working copy of it: flag
// 0x10 makes the copy anew from the Sim's own (and clears the source GUID), and a source's outfit is
// put on the copy, the source GUID kept unless it is the Sim's own. The Sim gets the outfit when the
// copy's lists have a top and a bottom, or a full body, for the category: person data 0x08 takes the
// outfit number and the scene draws it. Flag 0x08 then writes the copy back into the Sim's own age
// data. Always true.
bool Execution::changeOutfit(const bhav::Instruction& in, Frame& f) {
    constexpr std::uint8_t kFromStackObject = 0x01, kGuidInOperands = 0x02, kCategoryFromValue = 0x04, kCommit = 0x08,
                           kRenewCopy = 0x10, kClearSource = 0x20, kGuidFromTemps = 0x40;
    constexpr std::size_t kCurrentOutfit = 0x08, kSourceGuidLow = 0x77, kSourceGuidHigh = 0x78;
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
        personData(*sim, kSourceGuidLow) = 0;
        personData(*sim, kSourceGuidHigh) = 0;
    }
    // Without age data the game changes nothing more.
    const ObjectType* type = code_.objectType(sim->definition.guid());
    const auto* outfits = type != nullptr ? code_.ageData(type->group) : nullptr;
    if (outfits == nullptr) {
        return true;
    }
    // The Sim keeps a working copy of its outfits from its load. Flag 0x10 makes it anew.
    if (flags & kRenewCopy) {
        personData(*sim, kSourceGuidLow) = 0;
        personData(*sim, kSourceGuidHigh) = 0;
    }
    if (const auto& build = world().outfitBuild()) {
        const auto built = build(*sim, {change.sourceGuid.value_or(0), (flags & kRenewCopy) != 0, (flags & kCommit) != 0});
        if (built) {
            if (change.sourceGuid && built->sourceFound) {
                const std::uint32_t kept = *change.sourceGuid != sim->definition.guid() ? *change.sourceGuid : 0;
                personData(*sim, kSourceGuidLow) = static_cast<std::int16_t>(kept & 0xFFFF);
                personData(*sim, kSourceGuidHigh) = static_cast<std::int16_t>(kept >> 16);
            }
            if (hasClothesFor(built->lists, change.categoryFlag())) {
                personData(*sim, kCurrentOutfit) = change.category;
                sim->outfitChange = change;
            }
            return true;
        }
    }
    if (change.sourceGuid) {
        diag::warnOnce("Change Outfit: an outfit from a source isn't worn yet (PR-63)");
        return true;
    }
    // A category with no clothes leaves the Sim as it is. A Sim that aged here is checked in its new
    // age's lists.
    const auto aged = world().agedOutfitLists.find(sim->definition.guid());
    if (!hasClothesFor(aged != world().agedOutfitLists.end() ? aged->second : filledOutfitLists(*outfits), change.categoryFlag())) {
        return true;
    }
    personData(*sim, kCurrentOutfit) = change.category;
    sim->outfitChange = change;
    return true;
}

// Age (operand 0, a life stage) makes me that age at once: 0 adult, 1 child, 2 toddler, 3 teen,
// 4 elder, 0x0D baby, 0x0E young adult. Any other value does nothing. Person data 0x3A gets the
// age in years the game gives each stage (young adults count as 19, with person data 0xAF set),
// and the outfit source GUID (0x77-0x78) is cleared. Everything in my slots comes out and goes
// back into the same slot on the new body. The new age's data is built at once, and the scene
// builds the new body from it. Always true.
// Not done yet: the walk and run sets for the new age, the scripts the slot moves run, a pet's new
// routing size (PR-62).
bool Execution::age(const bhav::Instruction& in) {
    constexpr std::size_t kOutfitSourceLow = 0x77, kOutfitSourceHigh = 0x78;
    if (!me().isPerson()) {
        throw Stop{Outcome::Unsupported, "only a Sim can age", hex(me().definition.guid(), 8)};
    }
    // The years, and the age flag the scene dresses the Sim for (none for a baby, whose age data
    // the game leaves as it is).
    std::int16_t years = 0;
    std::optional<std::uint32_t> ageFlag;
    switch (in.operands[0]) {
    case 0x00: years = 19; ageFlag = 0x08; break;
    case 0x01: years = 3; ageFlag = 0x02; break;
    case 0x02: years = 2; ageFlag = 0x01; break;
    case 0x03: years = 16; ageFlag = 0x04; break;
    case 0x04: years = 51; ageFlag = 0x10; break;
    case 0x0D: years = 0; break;
    case 0x0E: years = 19; ageFlag = 0x40; break;
    default: return true;
    }
    SimObject& sim = me();
    std::int16_t& youngAdult = personData(sim, kYoungAdultPersonData);
    std::int16_t& sourceLow = personData(sim, kOutfitSourceLow);
    std::int16_t& sourceHigh = personData(sim, kOutfitSourceHigh);
    std::int16_t& ageYears = personData(sim, kNewAgePersonData);
    std::vector<std::pair<std::int16_t, std::int16_t>> held;  // (slot, object id)
    for (std::size_t i = 0; i < sim.slots.size(); ++i) {
        if (SimObject* object = sim.slots[i] != 0 ? world().find(sim.slots[i]) : nullptr) {
            held.emplace_back(static_cast<std::int16_t>(i), object->id);
            takeOutOfSlot(*object);
        }
    }
    // The new age's data becomes the Sim's own at once, so a Change Outfit right after reads it.
    if (ageFlag) {
        const auto& build = world().ageDataBuild();
        auto lists = build ? build(sim, *ageFlag) : std::nullopt;
        if (lists) {
            world().agedOutfitLists[sim.definition.guid()] = std::move(*lists);
        }
        if (lists || !build) {
            sim.newAge = *ageFlag;
        }
    }
    youngAdult = in.operands[0] == 0x0E ? 1 : 0;
    sourceLow = 0;
    sourceHigh = 0;
    ageYears = years;
    for (const auto& [slot, id] : held) {
        if (SimObject* object = world().find(id); object != nullptr && static_cast<std::size_t>(slot) < sim.slots.size()) {
            putInSlot(*object, sim, slot);
        }
    }
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
// Every satisfy goes into the Sim's want history, which leads the want trees to new wants. True
// when at least one of the Sim's wants (fears, for a fear) matched: each leaves an empty place and
// the Sim is queued for new ones, and the Sim gets their aspiration points. The definition's
// feedbackTree then shows the score. The influence they give and the extra want are not implemented.
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

    // The request the Sim's wants are compared with.
    swaf::Want request;
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

    auto satisfyFor = [&](std::int16_t neighbor) {
        // A pet (species not 0) has no wants, so nothing is recorded.
        if (world().neighbor(neighbor) == nullptr || world().neighborPersonValue(neighbor, kSpeciesPersonData) != 0) {
            return false;
        }
        request.id = static_cast<std::uint16_t>(neighbor);
        swaf::Want record = request;
        record.version = 10;
        record.tail = {0, 0, 0};
        const bool queued = world().wantsState().queue.contains(neighbor);
        auto& wants = world().neighborWants(neighbor);
        auto& fears = world().neighborFears(neighbor);
        bool satisfied = false;
        auto& list = def->isFear ? fears : wants;
        for (std::size_t at = 0; at < list.size(); ++at) {
            swaf::Want& w = list[at];
            if (w.guid == 0 || !wantMatches(w, request, def->op)) {
                continue;
            }
            record.tail[1] += w.tail.size() > 1 ? w.tail[1] : 0;
            record.tail[2] += w.tail.size() > 2 ? w.tail[2] : 0;
            w = {};
            forgetWantPlace(world(), neighbor, at, def->isFear);
            ++world().wantsState().satisfied[neighbor][at + (def->isFear ? 1000 : 0)];
            if (!satisfied && !queued) {
                for (auto* each : {&wants, &fears}) {
                    for (auto& other : *each) {
                        if (!other.tail.empty()) {
                            other.tail[0] = 0;
                        }
                    }
                }
            }
            satisfied = true;
        }
        const swaf::Want satisfiedRecord = record;
        addWantHistory(world(), neighbor, std::move(record));
        if (satisfied) {
            giveWantPoints(neighbor, satisfiedRecord.tail[1]);
            queueWantRefill(world(), neighbor, 3);
        }
        runWantFeedback(*def, neighbor, satisfiedRecord, satisfied, value);
        return satisfied;
    };
    if (def->simArrayTree.empty()) {
        return satisfyFor(sim);
    }
    // A want with a Sim list tree is satisfied for every Sim the tree lists for this Sim, and the
    // primitive is then false whatever they matched.
    for (const std::int16_t listed : wantSimList(def->simArrayTree, sim)) {
        satisfyFor(listed);
    }
    return false;
}

// The Sims a want's Sim list tree lists for `sim` (neighbor ids). The tree is one of the "Wants -
// Check Trees" object's, found by name as Run Tree by Name finds it. It runs at once on the lot's
// "Wants - Controller", on its temps, with the Sim as param 0. When it is true, the list is array 0
// of the object whose id it left in temp 0. Without either object, the tree or that array, no Sims.
std::vector<std::int16_t> Execution::wantSimList(const std::string& treeName, std::int16_t sim) {
    constexpr std::uint32_t kWantsControllerGuid = 0x6D9E58F0, kWantsCheckTreesGuid = 0x8D92DD2E;
    SimObject* controller = world().nextAfter(0, [](const SimObject& s) { return s.definition.guid() == kWantsControllerGuid; });
    const ObjectType* checkTrees = code_.objectType(kWantsCheckTreesGuid);
    if (controller == nullptr || checkTrees == nullptr || treeName.empty()) {
        return {};
    }
    const std::uint32_t group = checkTrees->group;
    std::optional<std::uint16_t> tree;
    for (const std::uint32_t g : {group, code_.semiGlobalGroup(group), kGlobalGroup}) {
        if ((tree = code_.behaviorByName(g, treeName))) {
            break;
        }
    }
    if (!tree) {
        return {};
    }
    const std::array<std::int16_t, 4> args{sim, 0, 0, 0};
    const RunResult r = runOnObject(*controller, group, *tree, controller->id, args);
    if (r.outcome == Outcome::Unsupported || r.outcome == Outcome::MissingCode) {
        throw Stop{r.outcome, r.reason, r.detail};
    }
    if (r.outcome != Outcome::True) {
        return {};
    }
    const SimObject* list = world().find(controller->thread.temps[0]);
    if (list == nullptr || list->arrays.empty()) {
        return {};
    }
    return list->arrays[0];
}

// A satisfied want's aspiration points. The score (person data 0xA2, in tens) stays within the age's
// levels, or -1000..16000 for an age without them, unless person data 0xC1 holds it. Person data 0xA0
// follows as its level on -100..100. The lifetime total (person data 0xA1) stays within 0..32767.
void Execution::giveWantPoints(std::int16_t neighbor, std::int32_t points) {
    constexpr std::size_t kLifeStage = 0x3A, kLevel = 0xA0, kLifetime = 0xA1, kScore = 0xA2, kHeld = 0xC1;
    constexpr std::int32_t kDefaultLowest = -1000, kDefaultHighest = 16000, kMostLifetime = 32767;
    const auto give = [&](auto& values) {
        if (values.size() <= kHeld) {
            return;
        }
        if (values[kHeld] == 0) {
            const auto& ages = world().wantDefinitions()->aspirationLevels;
            const auto age = ages.find(wantAgeCode(values[kLifeStage]));
            const std::int32_t lowest = age != ages.end() ? age->second.front() : kDefaultLowest;
            const std::int32_t highest = age != ages.end() ? age->second.back() : kDefaultHighest;
            const std::int32_t score = std::clamp(values[kScore] * 10 + points, lowest, highest) / 10;
            values[kScore] = static_cast<std::int16_t>(score);
            values[kLevel] = static_cast<std::int16_t>(age != ages.end() ? aspirationLevel(age->second, score * 10) : 0);
        }
        values[kLifetime] = static_cast<std::int16_t>(std::clamp(values[kLifetime] + points / 10, 0, kMostLifetime));
    };
    if (SimObject* sim = world().find(world().neighborData(neighbor, 0).value_or(0))) {
        give(sim->personData);
    } else if (auto* saved = world().neighborPersonData(neighbor)) {
        give(*saved);
    }
}

// Runs a want's feedbackTree, one of the "Wants - Check Trees" object's found by name, at once on the lot's
// "Wants - Controller" for a Sim on the lot. It runs for every satisfy, and its params are:
//   0     the Sim's neighbor id
//   1, 2  the want's GUID, low then high
//   3     the Sim it is about (neighbor id), 0 for other kinds
//   4, 5  the object, career or badge GUID it is about, low then high
//   6, 7  the record's argument and value
//   8     1 when one of the Sim's wants matched
//   9     its aspiration points
//   10    not known yet
//   11, 12  the score before the points and its level on -100..100
//   13    the value the primitive was given
//   14    its influence
// A tree that fails only costs its feedback, so the satisfied want stands.
void Execution::runWantFeedback(const content::WantDefinition& def, std::int16_t neighbor, const swaf::Want& record, bool satisfied, std::int16_t value) {
    constexpr std::uint32_t kWantsControllerGuid = 0x6D9E58F0, kWantsCheckTreesGuid = 0x8D92DD2E;
    constexpr std::size_t kLifeStage = 0x3A, kScore = 0xA2;
    SimObject* sim = world().find(world().neighborData(neighbor, 0).value_or(0));
    SimObject* controller = world().nextAfter(0, [](const SimObject& s) { return s.definition.guid() == kWantsControllerGuid; });
    const ObjectType* checkTrees = code_.objectType(kWantsCheckTreesGuid);
    if (def.feedbackTree.empty() || sim == nullptr || controller == nullptr || checkTrees == nullptr || sim->personData.size() <= kScore) {
        return;
    }
    std::optional<std::uint16_t> tree;
    for (const std::uint32_t g : {checkTrees->group, code_.semiGlobalGroup(checkTrees->group), kGlobalGroup}) {
        if ((tree = code_.behaviorByName(g, def.feedbackTree))) {
            break;
        }
    }
    if (!tree) {
        return;
    }
    const std::int32_t points = satisfied ? record.tail[1] : 0;
    const std::int32_t influence = satisfied ? record.tail[2] : 0;
    const std::int32_t before = sim->personData[kScore] * 10 - points;
    const auto& ages = world().wantDefinitions()->aspirationLevels;
    const auto age = ages.find(wantAgeCode(sim->personData[kLifeStage]));
    const auto kind = static_cast<content::WantArgument>(record.type);
    const bool longArgument = kind == content::WantArgument::Object || kind == content::WantArgument::Career || kind == content::WantArgument::Badge;
    const auto low = [](std::int32_t v) { return static_cast<std::int16_t>(v & 0xFFFF); };
    const auto high = [](std::int64_t v) { return static_cast<std::int16_t>(static_cast<std::uint32_t>(v) >> 16); };
    const std::array<std::int16_t, 15> params{
        neighbor,
        low(static_cast<std::int32_t>(record.guid)),
        high(record.guid),
        kind == content::WantArgument::Sim ? low(record.argument) : std::int16_t{0},
        longArgument ? low(record.argument) : std::int16_t{0},
        longArgument ? high(record.argument) : std::int16_t{0},
        low(record.argument),
        record.value,
        static_cast<std::int16_t>(satisfied ? 1 : 0),
        low(points),
        std::int16_t{0},
        low(before / 10),
        low(age != ages.end() ? aspirationLevel(age->second, before) : 0),
        value,
        low(influence),
    };
    const RunResult r = runOnObject(*controller, checkTrees->group, *tree, controller->id, params);
    if (r.outcome != Outcome::True && r.outcome != Outcome::False) {
        diag::warnOnce("a want's feedback tree stopped: " + def.feedbackTree + ": " + r.reason);
    }
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
    return sim::wantMatches(want, request, op, [this](std::uint32_t guid) {
        const ObjectType* type = code_.objectType(guid);
        return type != nullptr ? static_cast<std::int32_t>(type->definition.field(kWantCategoryField)) : -1;
    });
}

} // namespace ts2::sim::vm_detail
