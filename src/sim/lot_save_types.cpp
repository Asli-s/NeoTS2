#include "sim/lot_save_detail.hpp"
#include "formats/objd.hpp"
#include "sim/lot_simulation.hpp"
#include "sim/type_signature.hpp"

#include <algorithm>
#include <array>
#include <unordered_set>

namespace ts2::sim {

using namespace lot_save_detail;

namespace lot_save_detail {

objt::ObjectTypeTable typeTableRecord(const objt::ObjectTypeTable& loaded, const TypeOwners& owners,
                                      const std::unordered_set<std::uint32_t>& stale) {
    objt::ObjectTypeTable table = loaded;
    for (objt::Entry& entry : table.entries) {
        const auto now = owners.savedValues.find(entry.guid);
        if (now == owners.savedValues.end() || stale.contains(entry.guid)) {
            continue;
        }
        for (std::size_t i = 0; i < now->second.size(); ++i) {
            entry.unknown[2 * i] = static_cast<std::uint16_t>(now->second[i]);
            entry.unknown[2 * i + 1] = static_cast<std::uint16_t>(now->second[i] >> 16);
        }
    }
    return table;
}

} // namespace lot_save_detail

namespace {

// The entry the game writes in a lot's type table for an installed type: its GUID, the values a
// load compares (see savedTypeValues), its semi-global attribute count (the semi-global's attribute
// labels, string table 0x100, or 0 without a semi-global), its index, its kind (OBJD field 9), its
// catalog name, its flag (1 for a multi-tile part, 3 for the master, else 0) and the GUID at OBJD
// fields 0x1D and 0x1E. A part has no name, and a type whose catalog name is missing or empty is
// named by a star and its OBJD name.
objt::Entry typeEntry(CodeSource& code, const ObjectType& type, std::uint16_t index) {
    const objd::ObjectDefinition& d = type.definition;
    objt::Entry entry;
    entry.guid = d.guid();
    const auto values = savedTypeValues(code, type);
    std::uint32_t semi = 0;
    const std::uint32_t semiGlobal = code.semiGlobalGroup(type.group);
    if (semiGlobal != type.group) {
        constexpr std::uint16_t kAttributeLabels = 0x100;
        constexpr std::uint32_t kMaxLabels = 0x7FFF;
        while (semi < kMaxLabels && code.string(semiGlobal, kAttributeLabels, semi)) {
            ++semi;
        }
    }
    const std::array<std::uint32_t, 5> words{values[0], values[1], values[2], values[3], semi};
    for (std::size_t i = 0; i < words.size(); ++i) {
        entry.unknown[2 * i] = static_cast<std::uint16_t>(words[i]);
        entry.unknown[2 * i + 1] = static_cast<std::uint16_t>(words[i] >> 16);
    }
    entry.index = index;
    entry.kind = d.field(objd::field::kType);
    const bool part = d.field(objd::field::kMultiTileMasterId) != 0 && d.field(objd::field::kMultiTileSubIndex) != 0xFFFF;
    entry.flag = part ? 1 : d.isMultiTileMaster() ? 3 : 0;
    if (!part) {
        const auto catalog = code.catalogString(type.group, d.field(objd::field::kCatalogStringsId), 0);
        entry.name = catalog && !catalog->empty() ? *catalog : "*" + d.name;
    }
    constexpr std::size_t kSecondGuid = 208;
    entry.secondGuid = static_cast<std::uint32_t>(d.scriptField(kSecondGuid).value_or(0));
    return entry;
}

} // namespace

void addNewTypes(const dbpf::PackageImage& package, LotSimulation& simulation, TypeOwners& owners) {
    const auto types = typesOf(package);
    if (!types) {
        return;
    }
    std::unordered_set<std::uint32_t> known;
    std::uint16_t last = 0;
    for (const objt::Entry& entry : types->entries) {
        known.insert(entry.guid);
        last = std::max<std::uint16_t>(last, entry.index);
    }
    // Indices are kept in 16 bits and read back signed.
    constexpr std::uint16_t kLastIndex = 0x7FFF;
    simulation.world().nextAfter(0, [&](const SimObject& object) {
        const std::uint32_t guid = object.definition.guid();
        if (known.contains(guid) || last >= kLastIndex) {
            return false;
        }
        const ObjectType* type = simulation.code().objectType(guid);
        if (type == nullptr) {
            return false;
        }
        const objt::Entry entry = typeEntry(simulation.code(), *type, static_cast<std::uint16_t>(last + 1));
        // Flag 1 marks a multi-tile type, saved with a material block, which a new object can't start
        // from yet (see freshRecord). A Sim made this run is saved only with its character file.
        if ((entry.flag & 1) != 0 ||
            (entry.kind == objt::kPersonKind && !owners.writtenNewSims.contains(simulation.world().neighborIdOf(object)))) {
            return false;
        }
        known.insert(guid);
        ++last;
        owners.newTypes.push_back(entry);
        // A new Sim's type is named by its catalog text, whose first line is the Sim's first name.
        if (entry.kind == objt::kPersonKind) {
            owners.newTypes.back().name = simulation.world().neighborFirstName(simulation.world().neighborIdOf(object)).value_or("");
        }
        const auto index = static_cast<std::int16_t>(entry.index);
        owners.indexByGroup.emplace(type->group, index);
        owners.groupByIndex.emplace(index, type->group);
        owners.savedValues.emplace(guid, savedTypeValues(simulation.code(), *type));
        return false;
    });
}

} // namespace ts2::sim
