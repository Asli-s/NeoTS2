#include "sim/vm_execution.hpp"

namespace ts2::sim::vm_detail {

std::int16_t Execution::read(std::uint8_t owner, std::uint16_t data, Frame& f) {
    switch (owner) {
    case owner::kMyAttribute:              return slotAt(me().attributes, data, "attribute");
    case owner::kStackObjectAttribute:     return slotAt(stackData().attributes, data, "attribute");
    case owner::kStackObjectAttributeByParam:
        return slotAt(stackData().attributes, slotAt(f.params, data, "param"), "attribute");
    case owner::kMySemiAttribute:          return slotAt(me().semiAttributes, data, "semi-attribute");
    case owner::kStackObjectSemiAttribute: return slotAt(stackData().semiAttributes, data, "semi-attribute");
    case owner::kMyData:                   return slotAt(me().data, data, "object data");
    case owner::kStackObjectData:          return slotAt(stackData().data, data, "object data");
    case owner::kMyMotives:                return static_cast<std::int16_t>(motive(me(), data));
    case owner::kStackObjectMotives:       return static_cast<std::int16_t>(motive(stackData(), data));
    case owner::kStackObjectMotiveByTemp:
        return static_cast<std::int16_t>(motive(stackData(), slotAt(thread_.temps, data, "temp")));
    case owner::kMyPersonData:                return personData(me(), data);
    case owner::kStackObjectPersonData:       return personData(stackData(), data);
    case owner::kMyPersonDataByTemp:          return personData(me(), tempValue(data));
    case owner::kStackObjectPersonDataByTemp: return personData(stackData(), tempValue(data));
    case owner::kStackObjectDefinition:
        return static_cast<std::int16_t>(
            slotAt(stackData().definition.fields, data, "object definition field"));
    case owner::kGlobal:        return slotAt(globals_, data, "global");
    case owner::kLiteral:       return static_cast<std::int16_t>(data);
    case owner::kTemp:          return slotAt(thread_.temps, data, "temp");
    case owner::kParam:         return slotAt(f.params, data, "param");
    case owner::kStackObjectId: return stackObject();
    case owner::kTempByTemp:
        return slotAt(thread_.temps, slotAt(thread_.temps, data, "temp"), "temp[temp]");
    case owner::kLocal:         return slotAt(f.locals, data, "local");
    case owner::kConstant:      return constant(data, f.codeOwner);
    case owner::kConstantByTemp: return constantByTemp(data, f.codeOwner);
    case owner::kRoomByTemp:     return roomField(thread_.temps[0], data);
    case owner::kNeighborInStackObject: return neighborField(stackObject(), data);
    case owner::kNeighborPersonData:    return neighborPersonData(data);
    case owner::kNeighborObjectDefinition: return neighborDefinitionField(data);
    case owner::kTemporaryToken:        return temporaryTokenField(data);
    case owner::kMySlot:                return slotValue(me(), data);
    case owner::kStackObjectSlot:       return slotValue(stackData(), data);
    case owner::kMySlotByTemp:          return slotValue(me(), tempValue(data));
    case owner::kStackObjectSlotByTemp: return slotValue(stackData(), tempValue(data));
    case owner::kCheckTreeAdMin:        return testedAd(data).min;
    case owner::kCheckTreeAdRange:      return testedAd(data).delta;
    case owner::kCheckTreeAdPersonality: return personalityType(testedAd(data).type);
    case owner::kStackObjectHasFunction: return hasFunction(stackData(), data);
    case owner::kMyArrayIterator:            return arrayValue(me(), data, false);
    case owner::kStackObjectArrayIterator:   return arrayValue(stackData(), data, false);
    case owner::kMyArrayAtIterator:          return arrayValue(me(), data, true);
    case owner::kStackObjectArrayAtIterator: return arrayValue(stackData(), data, true);
    case owner::kMyArrayAtTemp:              return arrayElement(me(), data, thread_.temps[0]);
    case owner::kStackObjectArrayAtTemp:     return arrayElement(stackData(), data, thread_.temps[0]);
    default:
        throw Stop{Outcome::Unsupported, "data owner not implemented", hex(owner, 2)};
    }
}

// Room fields: 0 ambient light, 1 outside, 2 level, 3 area (counted in quarter tiles), 4 pool,
// 5 windows. Fields 1 and 7 are the room's region in room layers 0 and 1 (0 = outdoors).
std::int16_t Execution::roomField(std::int16_t roomId, std::uint16_t field) {
    const RoomInfo* room = world().rooms().room(roomId);
    if (room == nullptr) {
        throw Stop{Outcome::Unsupported, "room not known", std::to_string(roomId)};
    }
    switch (field) {
    case kRoomOutside:
    case kRoomLayer1: {
        const std::int32_t region = room->layerRegion[field == kRoomOutside ? 0 : 1];
        if (region == RoomMap::kNoRoom) {
            throw Stop{Outcome::Unsupported, "room has no region in that layer", std::to_string(roomId)};
        }
        // 1 when the layer's walls don't enclose the room (region 0), else 0.
        return region == 0 ? 1 : 0;
    }
    case kRoomLevel:   return static_cast<std::int16_t>(room->level + 1);
    case kRoomArea:    return static_cast<std::int16_t>(room->area());
    case kRoomPool: {
        // The game asks the region kind of one of the room's tiles. A room is all pool or none.
        const auto& grid = world().outside();
        for (std::int32_t x = 0; x < grid.width(); ++x) {
            for (std::int32_t y = 0; y < grid.height(); ++y) {
                for (int q = 0; q < 4; ++q) {
                    if (world().rooms().quarterRoom(room->level, x, y, q) == roomId) {
                        return grid.pool(room->level, x, y, q) ? 1 : 0;
                    }
                }
            }
        }
        return 0;
    }
    default:
        throw Stop{Outcome::Unsupported, "room field not implemented", std::to_string(field)};
    }
}

// The advertisement of motive `data` for my age group (adult, child, toddler, teen, elder, with babies
// using the adults') in the tested line, made (as zeros) when the line has none.
ttab::MotiveEffect& Execution::testedAd(std::uint16_t data) {
    constexpr std::uint16_t kMotives = 16;
    const SimObject& sim = me();
    if (testedLine == nullptr) {
        throw Stop{Outcome::Error, "no interaction tested for its advertisement", ""};
    }
    if (!sim.isPerson() || sim.personData[kSpeciesPersonData] != 0) {
        throw Stop{Outcome::Error, "advertisement for something that is not a human Sim", std::to_string(sim.id)};
    }
    if (data >= kMotives) {
        throw Stop{Outcome::Error, "advertisement motive out of range", std::to_string(data)};
    }
    const std::size_t group = advertisementGroup(sim);
    auto& groups = testedLine->motives;
    if (groups.size() <= group) {
        groups.resize(group + 1);
    }
    auto& effects = groups[group];
    if (effects.size() <= data) {
        effects.resize(data + 1u);
    }
    return effects[data];
}

std::int16_t Execution::personalityType(std::int16_t type) {
    if (type < 0 || type > 22) {
        throw Stop{Outcome::Error, "advertisement personality type out of range", std::to_string(type)};
    }
    return type;
}

std::int16_t Execution::hasFunction(const SimObject& object, std::uint16_t data) {
    const ObjectType* type = code_.objectType(object.definition.guid());
    const auto* functions = type ? code_.objectFunctions(*type) : nullptr;
    return functions != nullptr && data < functions->size() && (*functions)[data].action != 0 ? 1 : 0;
}

// Slot owners: the object in container slot [data], 0 when the slot is empty.
std::int16_t Execution::slotValue(SimObject& object, std::uint16_t data) {
    const auto slot = static_cast<std::int16_t>(data);
    if (slot < 0 || static_cast<std::size_t>(slot) >= object.slots.size()) {
        throw Stop{Outcome::Error, "slot number out of range", std::to_string(slot)};
    }
    return object.slots[static_cast<std::size_t>(slot)];
}

// Object array owners: the iterator of array [index], or the element it points at.
std::int16_t& Execution::arrayValue(SimObject& object, std::uint16_t data, bool elementAtIterator) {
    const auto index = static_cast<std::int16_t>(data);
    if (index < 0 || static_cast<std::size_t>(index) >= object.arrays.size()) {
        throw Stop{Outcome::Error, "bad object array", std::to_string(index)};
    }
    std::int16_t& iterator = object.arrayIterators[static_cast<std::size_t>(index)];
    if (!elementAtIterator) {
        return iterator;
    }
    auto& a = object.arrays[static_cast<std::size_t>(index)];
    if (iterator < 0 || static_cast<std::size_t>(iterator) >= a.size()) {
        throw Stop{Outcome::Error, "array iterator does not exist", std::to_string(iterator)};
    }
    return a[static_cast<std::size_t>(iterator)];
}

// Element `element` of an object's array `data`.
std::int16_t& Execution::arrayElement(SimObject& object, std::uint16_t data, std::int16_t element) {
    const auto index = static_cast<std::int16_t>(data);
    if (index < 0 || static_cast<std::size_t>(index) >= object.arrays.size()) {
        throw Stop{Outcome::Error, "bad object array", std::to_string(index)};
    }
    auto& a = object.arrays[static_cast<std::size_t>(index)];
    if (element < 0 || static_cast<std::size_t>(element) >= a.size()) {
        throw Stop{Outcome::Error, "array element does not exist", std::to_string(element)};
    }
    return a[static_cast<std::size_t>(element)];
}

// "Neighbor in stack object" fields, for the neighbor whose id is in the stack object (0 for a
// field above 10).
std::int16_t Execution::neighborField(std::int16_t neighborId, std::uint16_t field) {
    if (!world().neighbors()) {
        throw Stop{Outcome::Unsupported, "neighbor fields need the neighborhood", std::to_string(field)};
    }
    if (field >= World::kNeighborDataCount) {
        return 0;
    }
    const auto value = world().neighborData(neighborId, field);
    if (!value) {
        throw Stop{Outcome::Unsupported, "neighbor field not implemented", std::to_string(field)};
    }
    return *value;
}

// "Temporary Token": my token reference, fields 0-7 as SimObject::TokenSource lists them. The game
// keeps a pointer to the token itself in fields 8 and 9, which has no value here.
std::int16_t& Execution::temporaryTokenField(std::uint16_t data) {
    const auto field = static_cast<std::int16_t>(data);
    if (field < 0 || field > 9) {
        throw Stop{Outcome::Error, "temporary token field out of range", std::to_string(field)};
    }
    auto& t = me().tokenSource;
    switch (field) {
    case 0: return t.kind;
    case 1: return t.id;
    case 2: return t.counted;
    case 3: return t.index;
    case 4: return t.guidLow;
    case 5: return t.guidHigh;
    case 6: return t.flags;
    case 7: return t.propertyCount;
    default:
        throw Stop{Outcome::Unsupported, "temporary token fields 8 and 9 (the token's address) not implemented",
                   std::to_string(field)};
    }
}

// "Neighbor's object definition": field [data] of the object definition of the neighbor whose
// id is in the stack object.
std::int16_t Execution::neighborDefinitionField(std::uint16_t data) {
    if (!world().neighbors()) {
        throw Stop{Outcome::Unsupported, "neighbor fields need the neighborhood", std::to_string(data)};
    }
    const SavedNeighbor* neighbor = world().neighbor(stackObject());
    if (neighbor == nullptr) {
        throw Stop{Outcome::Error, "missing neighbor for data access", std::to_string(stackObject())};
    }
    const ObjectType* type = code_.objectType(neighbor->guid);
    if (type == nullptr || data >= type->definition.fields.size()) {
        throw Stop{Outcome::Error, "index into object definition out of range", std::to_string(data)};
    }
    return static_cast<std::int16_t>(type->definition.fields[data]);
}

// "Neighbor's person data": the stack object holds a neighbor id.
std::int16_t& Execution::neighborPersonData(std::uint16_t data) {
    if (!world().neighbors()) {
        throw Stop{Outcome::Unsupported, "neighbor person data needs the neighborhood", std::to_string(data)};
    }
    std::vector<std::int16_t>* values = world().neighborPersonData(stackObject());
    if (values == nullptr) {
        throw Stop{Outcome::Error, "missing neighbor for data access", std::to_string(stackObject())};
    }
    const auto index = static_cast<std::int16_t>(data);
    if (index < 0 || static_cast<std::size_t>(index) >= values->size()) {
        throw Stop{Outcome::Error, "person data field is out of range", std::to_string(index)};
    }
    return (*values)[static_cast<std::size_t>(index)];
}

void Execution::write(std::uint8_t owner, std::uint16_t data, Frame& f, std::int16_t value) {
    switch (owner) {
    case owner::kMyAttribute:              slotAt(me().attributes, data, "attribute") = value; return;
    case owner::kStackObjectAttribute:     slotAt(stackData().attributes, data, "attribute") = value; return;
    case owner::kStackObjectAttributeByParam:
        slotAt(stackData().attributes, slotAt(f.params, data, "param"), "attribute") = value;
        return;
    case owner::kMySemiAttribute:          slotAt(me().semiAttributes, data, "semi-attribute") = value; return;
    case owner::kStackObjectSemiAttribute: slotAt(stackData().semiAttributes, data, "semi-attribute") = value; return;
    case owner::kMyData:                   slotAt(me().data, data, "object data") = value; return;
    case owner::kStackObjectData:          slotAt(stackData().data, data, "object data") = value; return;
    case owner::kMyMotives:          setMotive(me(), data, value); return;
    case owner::kStackObjectMotives: setMotive(stackData(), data, value); return;
    case owner::kStackObjectMotiveByTemp:
        setMotive(stackData(), slotAt(thread_.temps, data, "temp"), value);
        return;
    case owner::kMyPersonData:                personData(me(), data) = value; return;
    case owner::kStackObjectPersonData:       personData(stackData(), data) = value; return;
    case owner::kMyPersonDataByTemp:          personData(me(), tempValue(data)) = value; return;
    case owner::kStackObjectPersonDataByTemp: personData(stackData(), tempValue(data)) = value; return;
    case owner::kStackObjectDefinition:
        // Game code writes these (e.g. "Function - Load"). Each object keeps its own copy.
        slotAt(stackData().definition.fields, data, "object definition field") =
            static_cast<std::uint16_t>(value);
        return;
    case owner::kGlobal:        slotAt(globals_, data, "global") = value; return;
    case owner::kTemp:          slotAt(thread_.temps, data, "temp") = value; return;
    case owner::kParam:         slotAt(f.params, data, "param") = value; return;
    case owner::kStackObjectId: stackObject() = value; return;
    case owner::kTempByTemp:
        slotAt(thread_.temps, slotAt(thread_.temps, data, "temp"), "temp[temp]") = value;
        return;
    case owner::kLocal:         slotAt(f.locals, data, "local") = value; return;
    case owner::kLiteral:
    case owner::kConstant:
    case owner::kConstantByTemp:
        throw Stop{Outcome::Error, "write to a read-only operand", hex(owner, 2)};
    case owner::kRoomByTemp:
        throw Stop{Outcome::Unsupported, "writing room fields not implemented", hex(owner, 2)};
    case owner::kNeighborInStackObject:
        throw Stop{Outcome::Unsupported, "writing neighbor fields not implemented", hex(owner, 2)};
    case owner::kNeighborPersonData: neighborPersonData(data) = value; return;
    case owner::kNeighborObjectDefinition:
        throw Stop{Outcome::Unsupported, "writing a neighbor's object definition not implemented", hex(owner, 2)};
    case owner::kTemporaryToken: temporaryTokenField(data) = value; return;
    case owner::kMySlot:
    case owner::kStackObjectSlot:
        throw Stop{Outcome::Unsupported, "writing slots not implemented", hex(owner, 2)};
    case owner::kCheckTreeAdMin:   testedAd(data).min = value; return;
    case owner::kCheckTreeAdRange: testedAd(data).delta = value; return;
    case owner::kCheckTreeAdPersonality: testedAd(data).type = personalityType(value); return;
    case owner::kMyArrayIterator:            arrayValue(me(), data, false) = value; return;
    case owner::kStackObjectArrayIterator:   arrayValue(stackData(), data, false) = value; return;
    case owner::kMyArrayAtIterator:          arrayValue(me(), data, true) = value; return;
    case owner::kStackObjectArrayAtIterator: arrayValue(stackData(), data, true) = value; return;
    case owner::kMyArrayAtTemp:              arrayElement(me(), data, thread_.temps[0]) = value; return;
    case owner::kStackObjectArrayAtTemp:     arrayElement(stackData(), data, thread_.temps[0]) = value; return;
    default:
        throw Stop{Outcome::Unsupported, "data owner not implemented", hex(owner, 2)};
    }
}

// Expression doesn't use operand byte 4 or bytes 8-15.
bool Execution::expression(const bhav::Instruction& in, Frame& f) {
    const auto x = bhav::decodeExpression(in);
    auto lhs = [&] { return read(x.lhsOwner, x.lhsData, f); };
    auto rhs = [&] { return read(x.rhsOwner, x.rhsData, f); };
    auto set = [&](int value) {
        const auto result = static_cast<std::int16_t>(value);
        write(x.lhsOwner, x.lhsData, f, result);
        return result;
    };
    auto nonZero = [](std::int16_t value) {
        if (value == 0) {
            throw Stop{Outcome::Error, "division by zero", ""};
        }
        return value;
    };

    switch (x.op) {
    case op::kGreater:             return lhs() > rhs();
    case op::kLess:                return lhs() < rhs();
    case op::kEquals:              return lhs() == rhs();
    case op::kGreaterOrEqual:      return lhs() >= rhs();
    case op::kLessOrEqual:         return lhs() <= rhs();
    case op::kNotEqual:            return lhs() != rhs();
    case op::kAdd:                 set(lhs() + rhs()); return true;
    case op::kSubtract:            set(lhs() - rhs()); return true;
    case op::kAssign:              set(rhs()); return true;
    case op::kMultiply:            set(lhs() * rhs()); return true;
    case op::kDivide:              { const auto d = nonZero(rhs()); set(lhs() / d); return true; }
    case op::kModulo:              { const auto d = nonZero(rhs()); set(lhs() % d); return true; }
    case op::kAnd:                 set(lhs() & rhs()); return true;
    case op::kOr:                  set(lhs() | rhs()); return true;
    case op::kXor:                 set(lhs() ^ rhs()); return true;
    case op::kAbs:                 set(std::abs(rhs())); return true;
    case op::kIsFlagSet:           return (lhs() & flagBit(rhs())) != 0;
    case op::kSetFlag:             set(lhs() | flagBit(rhs())); return true;
    case op::kClearFlag:           set(lhs() & ~flagBit(rhs())); return true;
    case op::kIncrementAndLess:    return set(lhs() + 1) < rhs();
    case op::kDecrementAndGreater: return set(lhs() - 1) > rhs();
    default:
        throw Stop{Outcome::Unsupported, "expression operator not implemented", hex(x.op, 2)};
    }
}

// Operands:
//   byte 1      operation
//   byte 2      whose arrays (0 = mine, else the stack object's)
//   bytes 3-4   array
//   byte 5, 6-7 value (owner, data)
//   byte 8, 9-10 iterator (owner, data)
// Only "find" can return false.
bool Execution::arrayOperation(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    SimObject* object = &me();
    if (o[2] != 0) {
        object = world().find(stackObject());
        if (object == nullptr) {
            throw Stop{Outcome::Error, "stack number out of range", std::to_string(stackObject())};
        }
    }
    const auto arrayIndex = static_cast<std::int16_t>(o[3] | o[4] << 8);
    if (arrayIndex < 0 || static_cast<std::size_t>(arrayIndex) >= object->arrays.size()) {
        throw Stop{Outcome::Error, "bad object array", std::to_string(arrayIndex)};
    }
    std::vector<std::int16_t>& a = object->arrays[static_cast<std::size_t>(arrayIndex)];
    const std::uint8_t valueOwner = o[5];
    const auto valueData = static_cast<std::uint16_t>(o[6] | o[7] << 8);
    const std::uint8_t iteratorOwner = o[8];
    const auto iteratorData = static_cast<std::uint16_t>(o[9] | o[10] << 8);
    auto iterator = [&]() {
        const std::int16_t i = read(iteratorOwner, iteratorData, f);
        if (i < 0 || static_cast<std::size_t>(i) >= a.size()) {
            throw Stop{Outcome::Error, "array iterator does not exist", std::to_string(i)};
        }
        return static_cast<std::size_t>(i);
    };
    auto requireElements = [&]() {
        if (a.empty()) {
            throw Stop{Outcome::Error, "array iterator does not exist", "empty array"};
        }
    };
    // Sizes are read back as 16-bit numbers, so an array never grows past the largest one.
    auto requireRoom = [&]() {
        if (a.size() >= static_cast<std::size_t>(INT16_MAX)) {
            throw Stop{Outcome::Error, "array is full", std::to_string(a.size())};
        }
    };

    switch (o[1]) {
    case kArrayClear:
        a.clear();
        return true;
    case kArraySize:
        write(valueOwner, valueData, f, static_cast<std::int16_t>(a.size()));
        return true;
    case kArrayResize: {
        const std::int16_t size = read(valueOwner, valueData, f);
        if (size < 0) {
            throw Stop{Outcome::Unsupported, "array resize to a negative size", std::to_string(size)};
        }
        a.resize(static_cast<std::size_t>(size), 0);
        return true;
    }
    case kArrayFill:
        std::fill(a.begin(), a.end(), read(valueOwner, valueData, f));
        return true;
    case kArrayInsertFront:
        requireRoom();
        a.insert(a.begin(), read(valueOwner, valueData, f));
        return true;
    case kArrayInsertBack:
        requireRoom();
        a.push_back(read(valueOwner, valueData, f));
        return true;
    case kArrayInsertAtIterator: {
        requireRoom();
        const std::int16_t value = read(valueOwner, valueData, f);
        a.insert(a.begin() + static_cast<std::ptrdiff_t>(iterator()), value);
        return true;
    }
    case kArrayRemoveFront:
        requireElements();
        a.erase(a.begin());
        return true;
    case kArrayRemoveBack:
        requireElements();
        a.pop_back();
        return true;
    case kArrayRemoveAtIterator:
        a.erase(a.begin() + static_cast<std::ptrdiff_t>(iterator()));
        return true;
    case kArrayFind: {
        const std::int16_t value = read(valueOwner, valueData, f);
        const std::int16_t start = read(iteratorOwner, iteratorData, f);
        if (start < 0 || static_cast<std::size_t>(start) >= a.size()) {
            return false;
        }
        const auto it = std::find(a.begin() + start, a.end(), value);
        if (it == a.end()) {
            return false;
        }
        write(iteratorOwner, iteratorData, f, static_cast<std::int16_t>(it - a.begin()));
        return true;
    }
    case kArraySwap: {
        const std::int16_t first = read(valueOwner, valueData, f);
        if (first < 0 || static_cast<std::size_t>(first) >= a.size()) {
            throw Stop{Outcome::Error, "array iterator does not exist", std::to_string(first)};
        }
        std::swap(a[static_cast<std::size_t>(first)], a[iterator()]);
        return true;
    }
    case kArraySortDescending:
        std::stable_sort(a.begin(), a.end(), std::greater<>());
        return true;
    case kArraySortAscending:
        std::stable_sort(a.begin(), a.end());
        return true;
    default:
        throw Stop{Outcome::Error, "unknown array operation", std::to_string(o[1])};
    }
}

// Constant references: bits 13-14 pick the range (own, semi-global, global, and 3 is invalid), bits
// 7-12 plus bit 15 the table, bits 0-6 the value.
std::int16_t Execution::constant(std::uint16_t data, std::uint32_t codeOwner) {
    const std::uint32_t range = (data >> 13) & 3;
    if (range == 3) {
        throw Stop{Outcome::Error, "invalid constant", hex(data, 4)};
    }
    const std::uint32_t table = ((data >> 7) & 0x3F) | ((data >> 9) & 0x40);
    return slotAt(constantTable(range * 0x80 + table, 0x80, codeOwner), data & 0x7F, "constant");
}

// "Constants [temp]": data >> 3 picks the table, temp [data & 7] the value.
std::int16_t Execution::constantByTemp(std::uint16_t data, std::uint32_t codeOwner) {
    return slotAt(constantTable(data >> 3, 0x400, codeOwner), thread_.temps[data & 7], "constant");
}

// Table ids by range: the code owner's own tables (0x1000+), its semi-global tables (0x2000+) and
// the global tables (0x100+).
const std::vector<std::int16_t>& Execution::constantTable(std::uint32_t id, std::uint32_t rangeSize,
                                               std::uint32_t codeOwner) {
    std::uint32_t group = kGlobalGroup;
    std::uint32_t instance = 0x0100 + (id - 2 * rangeSize);
    if (id < rangeSize) {
        group = codeOwner;
        instance = 0x1000 + id;
    } else if (id < 2 * rangeSize) {
        group = code_.semiGlobalGroup(codeOwner);
        instance = 0x2000 + (id - rangeSize);
    }
    const std::vector<std::int16_t>* values =
        code_.constants(group, static_cast<std::uint16_t>(instance));
    if (values == nullptr) {
        throw Stop{Outcome::MissingCode, "constant table not found",
                   hex(group, 8) + " " + hex(instance, 4)};
    }
    return *values;
}

} // namespace ts2::sim::vm_detail
