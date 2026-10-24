#include "sim/vm_execution.hpp"

namespace ts2::sim::vm_detail {

// Manage Inventory. Byte 0: the inventory (bits 0-2: global, lot, family, neighbor, game-wide,
// bit 3: counted tokens), its id from owner byte 1 / data 2-3. Byte 4 is the operation, bytes 5-8
// a GUID (0: the stack object's, for the operations that need one). Byte 9 with byte 0 bits
// 4-5 gives a flags value and mask for singular tokens ((flags & mask) == value, and marked tokens
// never match), and the kind of a counted token. Owner byte 10 / data 11-12 is an index
// (counted from 1), owner byte 13 / data 14-15 a value or the result, owner byte 6 / data 7-8 a
// second index.
bool Execution::manageInventory(const bhav::Instruction& in, Frame& f) {
    const auto& o = in.operands;
    const std::uint8_t op = o[4];
    SimObject& self = me();
    bool counted = (o[0] & 0x08) != 0;
    int kind = o[0] & 0x07;
    std::int16_t id = 0;
    if (counted || op != 9) {
        id = read(o[1], word(o, 2), f);
    } else {
        kind = self.tokenSource.kind;
        id = self.tokenSource.id;
        counted = self.tokenSource.counted != 0;
    }
    Inventory* inventory = kind <= 4 ? world().inventory(static_cast<InventoryKind>(kind), id) : nullptr;
    if (inventory == nullptr) {
        throw Stop{Outcome::Error, "inventory not found", std::to_string(kind) + " " + std::to_string(id)};
    }

    std::uint32_t guid = static_cast<std::uint32_t>(o[5] | o[6] << 8 | o[7] << 16) | static_cast<std::uint32_t>(o[8]) << 24;
    const bool needsGuid = counted ? (op == 0 || op == 2 || op == 4 || op == 6 || op == 7)
                                   : (op == 0 || op == 2 || op == 3);
    if (guid == 0 && needsGuid) {
        const SimObject* stackObject = world().find(this->stackObject());
        if (stackObject == nullptr) {
            throw Stop{Outcome::Error, "stack object not found for inventory", std::to_string(this->stackObject())};
        }
        guid = stackObject->definition.guid();
    }
    const auto index = [&] { return static_cast<std::int16_t>(read(o[10], word(o, 11), f) - 1); };
    const auto value = [&] { return read(o[13], word(o, 14), f); };
    const auto result = [&](std::int32_t v) { write(o[13], word(o, 14), f, checkedResult(v)); };
    return counted ? countedTokens(*inventory, op, guid, o, index, value, result, f, kind, id)
                   : singularTokens(*inventory, op, guid, o, index, value, result, f, kind, id);
}

std::uint16_t Execution::word(const std::array<std::uint8_t, 16>& o, std::size_t at) {
    return static_cast<std::uint16_t>(o[at] | o[at + 1] << 8);
}

// Results outside 16 bits are error 0x14 in the game.
std::int16_t Execution::checkedResult(std::int32_t v) {
    if (v < std::numeric_limits<std::int16_t>::min() || v > std::numeric_limits<std::int16_t>::max()) {
        throw Stop{Outcome::Error, "data value out of range", std::to_string(v)};
    }
    return static_cast<std::int16_t>(v);
}

// Remembers where the object's token came from, for writing it back (operation 9).
void Execution::copyToObject(SimObject& self, const InventoryToken& t, int kind, std::int16_t id, bool counted,
                  std::size_t at) {
    self.token = t;
    self.tokenSource = {static_cast<std::int16_t>(kind), id, static_cast<std::int16_t>(counted ? 1 : 0),
                        static_cast<std::int16_t>(at), static_cast<std::int16_t>(t.guid & 0xFFFF),
                        static_cast<std::int16_t>(t.guid >> 16), static_cast<std::int16_t>(t.flags),
                        static_cast<std::int16_t>(std::min<std::size_t>(t.properties.size(), 0x7FFF))};
}

std::int16_t Execution::property(const InventoryToken& t, std::int16_t i) {
    return i >= 0 && static_cast<std::size_t>(i) < t.properties.size() ? t.properties[static_cast<std::size_t>(i)] : 0;
}

// Setting a property past the end first fills the list with zeros.
void Execution::setProperty(InventoryToken& t, std::int16_t i, std::int16_t v) {
    if (i < 0) {
        return;
    }
    if (static_cast<std::size_t>(i) >= t.properties.size()) {
        t.properties.resize(static_cast<std::size_t>(i) + 1, 0);
    }
    t.properties[static_cast<std::size_t>(i)] = v;
}

// The search singular-token operations use: forward from the 1-based position `start` (so the
// last result continues the search) or backward before it. It finds unmarked tokens whose flags match
// and whose category is 0, and with a GUID, of that object type.
std::optional<std::size_t> Execution::findToken(const TokenList& tokens, std::optional<std::uint32_t> guid,
                                            std::int16_t start, std::uint16_t value, std::uint16_t mask,
                                            bool backward) {
    auto matches = [&](const InventoryToken& t) {
        return (!guid || t.guid == *guid) && (t.flags & 0x04) == 0 && (value & mask) == (t.flags & mask) &&
               t.category == 0;
    };
    const auto n = static_cast<std::int32_t>(tokens.size());
    if (!backward) {
        for (std::int32_t i = std::max<std::int32_t>(start, 0); i < n; ++i) {
            if (matches(*tokens[static_cast<std::size_t>(i)])) {
                return static_cast<std::size_t>(i);
            }
        }
        return std::nullopt;
    }
    const std::int32_t from = (start - 1 >= 0 && start - 1 < n) ? start - 2 : n - 1;
    for (std::int32_t i = from; i >= 0; --i) {
        if (matches(*tokens[static_cast<std::size_t>(i)])) {
            return static_cast<std::size_t>(i);
        }
    }
    return std::nullopt;
}

template <typename Index, typename Value, typename Result>
bool Execution::singularTokens(Inventory& inventory, std::uint8_t op, std::uint32_t guid, const std::array<std::uint8_t, 16>& o,
                    Index index, Value value, Result result, Frame& f, int kind, std::int16_t id) {
    TokenList& tokens = inventory.singular;
    SimObject& self = me();
    const auto flagsValue = static_cast<std::uint16_t>((o[9] & 0x03) << 3 | (o[0] >> 4 & 0x03));
    const auto flagsMask = static_cast<std::uint16_t>((o[9] & 0x7C) >> 2);
    const bool backward = (o[0] & 0x80) != 0;
    auto at = [&](std::int16_t i) -> InventoryToken* {
        return i >= 0 && static_cast<std::size_t>(i) < tokens.size() ? tokens[static_cast<std::size_t>(i)].get() : nullptr;
    };
    switch (op) {
    case 0:  // add a token of the GUID with the flags value
        if (tokens.size() > 0x7FFE) {
            return false;
        }
        tokens.push_back(std::make_shared<InventoryToken>(InventoryToken{guid, flagsValue, 0, 0, 0, {}}));
        return true;
    case 1: {  // remove the token at the index
        const std::int16_t i = index();
        if (at(i) == nullptr) {
            return false;
        }
        tokens.erase(tokens.begin() + i);
        return true;
    }
    case 2:  // remove every matching token of the GUID
        return std::erase_if(tokens, [&](const auto& t) {
                   return t->guid == guid && (t->flags & 0x04) == 0 && (flagsValue & flagsMask) == (t->flags & flagsMask) &&
                          t->category == 0;
               }) != 0;
    case 3:    // find a token of the GUID
    case 12: {  // find a token by flags only, its position goes back into the index
        const auto start = static_cast<std::int16_t>(read(o[10], word(o, 11), f));
        const auto found = findToken(tokens, op == 3 ? std::optional{guid} : std::nullopt, start, flagsValue, flagsMask, backward);
        if (!found) {
            return false;
        }
        write(o[10], word(o, 11), f, checkedResult(static_cast<std::int32_t>(*found) + 1));
        return true;
    }
    case 4: {  // push a value onto the token's properties
        InventoryToken* t = at(index());
        if (t == nullptr || t->properties.size() >= 0x7FFF) {
            return false;
        }
        t->properties.push_back(value());
        return true;
    }
    case 5: {  // pop the token's last property
        InventoryToken* t = at(index());
        if (t == nullptr || t->properties.empty()) {
            return false;
        }
        const std::int16_t last = t->properties.back();
        t->properties.pop_back();
        result(last);
        return true;
    }
    case 6: {  // copy the token into the object's token
        const std::int16_t i = index();
        InventoryToken* t = at(i);
        if (t == nullptr) {
            return false;
        }
        copyToObject(self, *t, kind, id, false, static_cast<std::size_t>(i));
        return true;
    }
    case 7: {  // read a property of the object's token
        const std::int16_t i = index();
        if (i < 0 || static_cast<std::size_t>(i) >= self.token.properties.size()) {
            return false;
        }
        result(self.token.properties[static_cast<std::size_t>(i)]);
        return true;
    }
    case 8: {  // set a property of the object's token
        const std::int16_t i = index();
        if (i < 0 || static_cast<std::size_t>(i) >= self.token.properties.size()) {
            return false;
        }
        self.token.properties[static_cast<std::size_t>(i)] = value();
        return true;
    }
    case 9: {  // write the object's token back where it came from
        InventoryToken* t = at(self.tokenSource.index);
        if (t == nullptr) {
            return false;
        }
        if (t->value2 != self.token.value2) {
            if (self.token.value2 == 0) {
                throw Stop{Outcome::Error, "object token has no object link", ""};
            }
            if (t->value2 != 0) {
                return false;
            }
            throw Stop{Outcome::Error, "inventory token has no object link", ""};
        }
        *t = self.token;
        return true;
    }
    case 10:  // how many tokens there are
        result(static_cast<std::int32_t>(tokens.size()));
        return true;
    case 11: {  // add a copy of the object's token, with the flags it was copied with
        if (tokens.size() > 0x7FFE) {  // as op 0: past this a token has no 16-bit index
            return false;
        }
        if (self.token.value2 != 0) {
            throw Stop{Outcome::Error, "object token has an object link", ""};
        }
        auto copy = std::make_shared<InventoryToken>(self.token);
        copy->flags = static_cast<std::uint16_t>(self.tokenSource.flags);
        tokens.push_back(std::move(copy));
        return true;
    }
    case 13: {  // count the tokens matching the flags
        const auto n = std::count_if(tokens.begin(), tokens.end(), [&](const auto& t) {
            return (t->flags & 0x04) == 0 && (flagsValue & flagsMask) == (t->flags & flagsMask) && t->category == 0;
        });
        result(static_cast<std::int32_t>(n));
        return true;
    }
    case 14: {  // set property [index] of the token at the second index
        InventoryToken* t = at(static_cast<std::int16_t>(read(o[6], word(o, 7), f) - 1));
        const std::int16_t i = index();
        const std::int16_t v = value();
        if (t == nullptr) {
            return false;
        }
        setProperty(*t, i, v);
        return true;
    }
    case 15: {  // read property [index] of the token at the second index (0 past the end)
        InventoryToken* t = at(static_cast<std::int16_t>(read(o[6], word(o, 7), f) - 1));
        const std::int16_t i = index();
        if (t == nullptr) {
            return false;
        }
        result(property(*t, i));
        return true;
    }
    case 16:
    case 19: {  // put the stack object away: a visible token of its type with its archive's id
        const SimObject* object = world().find(stackObject());
        if (object == nullptr || tokens.size() > 0x7FFE) {
            return false;
        }
        InventoryToken t;
        t.guid = object->definition.guid();
        t.flags = 1;
        t.value2 = world().nextArchiveId();
        tokens.push_back(std::make_shared<InventoryToken>(std::move(t)));
        return true;
    }
    default:
        throw Stop{Outcome::Unsupported, "Manage Inventory operation not implemented", hex(op, 2)};
    }
}

// Counted tokens are kept per GUID and kind (property 1, from byte 9). Property 0 is the count.
template <typename Index, typename Value, typename Result>
bool Execution::countedTokens(Inventory& inventory, std::uint8_t op, std::uint32_t guid, const std::array<std::uint8_t, 16>& o,
                   Index index, Value value, Result result, Frame& f, int kind, std::int16_t id) {
    TokenList& tokens = inventory.counted;
    SimObject& self = me();
    const std::uint8_t tokenKind = o[9];
    const auto newFlags = static_cast<std::uint16_t>((o[0] & 0x70) >> 4);
    auto find = [&](std::uint32_t g, std::int16_t k) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            if (tokens[i]->guid == g && property(*tokens[i], 1) == k) {
                return i;
            }
        }
        return std::nullopt;
    };
    auto add = [&](std::uint32_t g, std::int16_t k, std::uint16_t flags, std::int16_t n) {
        if (n < 1) {
            return false;
        }
        if (const auto i = find(g, k)) {
            auto& t = *tokens[*i];
            setProperty(t, 0, static_cast<std::int16_t>(std::min<std::int32_t>(property(t, 0) + n, 0x7FFF)));
        } else {
            tokens.push_back(std::make_shared<InventoryToken>(InventoryToken{g, flags, 0, 0, 0, {n, k}}));
        }
        return true;
    };
    auto remove = [&](std::uint32_t g, std::int16_t k, std::int16_t n) {
        if (n < 1) {
            return false;
        }
        const auto i = find(g, k);
        if (!i) {
            return false;
        }
        auto& t = *tokens[*i];
        const std::int32_t left = property(t, 0) - n;
        if (left > 0) {
            setProperty(t, 0, static_cast<std::int16_t>(left));
        } else {
            tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(*i));
        }
        return true;
    };
    auto at = [&](std::int16_t i) -> InventoryToken* {
        return i >= 0 && static_cast<std::size_t>(i) < tokens.size() ? tokens[static_cast<std::size_t>(i)].get() : nullptr;
    };
    switch (op) {
    case 0:  // add [value] of the GUID and kind
        return add(guid, tokenKind, newFlags, value());
    case 1: {  // add [value] to the token at the index
        const InventoryToken* t = at(index());
        const std::int16_t n = value();
        if (t == nullptr) {
            return false;
        }
        add(t->guid, property(*t, 1), newFlags, n);
        return true;
    }
    case 2:  // remove [value] of the GUID and kind
        return remove(guid, tokenKind, value());
    case 3: {  // remove [value] from the token at the index
        const InventoryToken* t = at(index());
        const std::int16_t n = value();
        if (t == nullptr) {
            return false;
        }
        remove(t->guid, property(*t, 1), n);
        return true;
    }
    case 4: {  // remove the token of the GUID and kind
        const auto i = find(guid, tokenKind);
        if (!i) {
            return false;
        }
        tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(*i));
        return true;
    }
    case 5: {  // remove the token at the index
        const std::int16_t i = index();
        if (i < 0) {
            throw Stop{Outcome::Error, "inventory index out of range", std::to_string(i)};
        }
        if (at(i) == nullptr) {
            return false;
        }
        tokens.erase(tokens.begin() + i);
        return true;
    }
    case 6: {  // find the token of the GUID and kind: its count, and its position into the index
        const auto i = find(guid, tokenKind);
        if (!i) {
            return false;
        }
        result(property(*tokens[*i], 0));
        write(o[10], word(o, 11), f, checkedResult(static_cast<std::int32_t>(*i) + 1));
        return true;
    }
    case 7: {  // copy the token of the GUID and kind into the object's token
        const auto i = find(guid, tokenKind);
        if (!i) {
            return false;
        }
        copyToObject(self, *tokens[*i], kind, id, true, *i);
        return true;
    }
    case 8: {  // copy the token at the index into the object's token
        const std::int16_t i = index();
        const InventoryToken* t = at(i);
        if (t == nullptr) {
            return false;
        }
        copyToObject(self, *t, kind, id, true, static_cast<std::size_t>(i));
        return true;
    }
    case 9: {  // the next token of the kind after the 1-based position at the index: its position
               // goes back to the index, its GUID (low, then high) to the value and the slot after it
        const auto start = static_cast<std::int32_t>(read(o[10], word(o, 11), f));
        for (std::size_t i = static_cast<std::size_t>(std::max(start, 0)); i < tokens.size(); ++i) {
            if (property(*tokens[i], 1) == static_cast<std::int16_t>(tokenKind)) {
                write(o[10], word(o, 11), f, checkedResult(static_cast<std::int32_t>(i) + 1));
                write(o[13], word(o, 14), f, static_cast<std::int16_t>(tokens[i]->guid & 0xFFFF));
                write(o[13], static_cast<std::uint16_t>(word(o, 14) + 1), f, static_cast<std::int16_t>(tokens[i]->guid >> 16));
                return true;
            }
        }
        return false;
    }
    case 10:  // how many counted tokens there are
        result(static_cast<std::int32_t>(tokens.size()));
        return true;
    default:
        throw Stop{Outcome::Unsupported, "Manage Inventory operation not implemented", "counted " + hex(op, 2)};
    }
}

} // namespace ts2::sim::vm_detail
