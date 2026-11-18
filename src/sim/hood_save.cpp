#include "sim/hood_save.hpp"

#include "formats/famt.hpp"
#include "formats/fami.hpp"
#include "formats/lot_records.hpp"
#include "formats/lot_description.hpp"
#include "formats/ngbh.hpp"
#include "formats/qfs.hpp"
#include "formats/sdsc.hpp"
#include "formats/srel.hpp"
#include "sim/clock.hpp"
#include "sim/hood_save_detail.hpp"
#include "sim/relationship_table.hpp"
#include "sim/world.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace ts2::sim {

namespace {

// The person data a description keeps as its own: the lot's running interaction and queue state,
// routing, look-at, selection and the like. Most come from the game's own saves watched as they
// were made. 0x0A, 0x14, 0x18, 0x1C, 0x1D, 0x20, 0x22, 0x29, 0x2A, 0x2C, 0x34, 0x38, 0x39, 0x4B,
// 0x4C, 0x68, 0x9D, 0xA4 and 0xD0 come only from saved files, where a neighborhood change made
// later cannot be told apart.
constexpr std::array<std::uint8_t, 48> kKeptByDescription{
    0x00, 0x08, 0x0A, 0x13, 0x14, 0x15, 0x18, 0x19, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22,
    0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3B, 0x3E, 0x42, 0x4B, 0x4C, 0x4F, 0x50, 0x66, 0x67, 0x68, 0x9D, 0x9E, 0xA3, 0xA4, 0xA6, 0xD0};

using hoodsave::bodyOf;

} // namespace

bool descriptionTakesPersonData(std::size_t index) {
    return std::find(kKeptByDescription.begin(), kKeptByDescription.end(), index) == kKeptByDescription.end();
}

std::array<std::int16_t, 231> savedDescriptionPersonData(std::span<const std::int16_t> description,
                                                         std::span<const std::int16_t> sim) {
    std::array<std::int16_t, 231> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::span<const std::int16_t> from = descriptionTakesPersonData(i) ? sim : description;
        out[i] = i < from.size() ? from[i] : std::int16_t{0};
    }
    return out;
}

HoodSaveCounts fillNeighborhoodPackage(dbpf::PackageImage& hood, const dbpf::PackageImage& lot, std::uint32_t lotId,
                                       const World& world, std::span<const std::int16_t> globals, NewNeighbors newNeighbors) {
    HoodSaveCounts counts;
    // Neighbors made this run that the save leaves out, with every record naming them.
    const bool leaveOut = newNeighbors == NewNeighbors::LeftOut;
    const auto isLeftOut = [&](std::int16_t id) { return leaveOut && world.addedNeighbors().contains(id); };
    std::set<std::uint32_t> leftOutGuids;
    if (leaveOut) {
        for (const std::int16_t id : world.addedNeighbors()) {
            if (const SavedNeighbor* n = world.neighbor(id)) {
                leftOutGuids.insert(n->guid);
            }
        }
    }
    const std::optional<std::uint32_t> household = world.lotHousehold();
    // The Sims on the lot, by neighbor id.
    std::unordered_map<std::int16_t, const SimObject*> onLot;
    world.nextAfter(0, [&](const SimObject& object) {
        if (object.isPerson()) {
            if (const std::int16_t id = world.neighborIdOf(object); id > 0) {
                onLot.emplace(id, &object);
            }
        }
        return false;
    });
    std::optional<nhood::LotInfoRecord> lotInfo;
    for (const dbpf::StoredResource& resource : lot.resources) {
        if (resource.key.type == nhood::kLotInfoType) {
            lotInfo = nhood::readLotInfo(bodyOf(resource));
            break;
        }
    }
    // A neighbor made this run gets its description and DNA first, and is then saved like the others.
    if (!leaveOut) {
        counts.newNeighbors = hoodsave::addNewDescriptions(hood, world);
        counts.newDna = hoodsave::addNewDna(hood, world);
    }
    std::map<std::uint32_t, std::size_t> relationshipRecords;
    for (std::size_t i = 0; i < hood.resources.size(); ++i) {
        if (hood.resources[i].key.type == srel::kType) {
            relationshipRecords.emplace(hood.resources[i].key.instance, i);
        }
    }
    const auto instanceOf = [](std::int16_t from, std::int16_t to) {
        return static_cast<std::uint32_t>(static_cast<std::uint16_t>(from)) << 16 | static_cast<std::uint16_t>(to);
    };
    // Each relationship record takes the values the neighbor holds now. A neighbor whose
    // relationships changed has its description saved again too, as the game does.
    std::set<std::int16_t> changed;
    std::map<std::int16_t, std::vector<std::int16_t>> made;  // new records by neighbor, unordered
    if (const std::set<std::int16_t>* neighbors = world.neighbors()) {
        for (const std::int16_t from : *neighbors) {
            for (const std::int16_t to : world.neighborRelationshipTargets(from)) {
                const std::vector<std::int32_t>* values = world.neighborRelationshipValues(from, to);
                if (values == nullptr || isLeftOut(from) || isLeftOut(to)) {
                    continue;
                }
                const auto at = relationshipRecords.find(instanceOf(from, to));
                if (at == relationshipRecords.end()) {
                    // The game seldom saves an empty record (2 of 34,819 in played neighborhoods). A
                    // script's failed lookup leaves one here, so only one holding values is saved.
                    if (!values->empty()) {
                        made[from].push_back(to);
                        changed.insert(from);
                    }
                    continue;
                }
                dbpf::StoredResource& resource = hood.resources[at->second];
                std::optional<srel::Record> record = srel::readRecord(bodyOf(resource));
                if (!record || record->values == *values) {
                    continue;
                }
                record->values = *values;
                resource = dbpf::storeResource(resource.key, srel::writeRecord(*record));
                ++counts.relationships;
                changed.insert(from);
            }
        }
    }
    std::vector<dbpf::StoredResource> madeRecords;
    for (dbpf::StoredResource& resource : hood.resources) {
        if (resource.key.type == sdsc::kType) {
            const auto id = static_cast<std::int16_t>(resource.key.instance);
            const auto sim = onLot.find(id);
            if (sim == onLot.end() && !changed.contains(id)) {
                continue;
            }
            std::optional<sdsc::Record> record = sdsc::readRecord(bodyOf(resource));
            if (!record) {
                continue;
            }
            // The neighbor's working copy holds what scripts changed in it while the lot ran.
            std::vector<std::int16_t> description(record->personData.begin(), record->personData.end());
            for (std::size_t i = 0; i < description.size(); ++i) {
                description[i] = world.neighborPersonValue(id, i);
            }
            if (sim != onLot.end()) {
                const auto& own = sim->second->personData;
                record->personData = savedDescriptionPersonData(description, std::span<const std::int16_t>(own.begin(), own.size()));
            } else {
                std::copy_n(description.begin(), std::min(description.size(), record->personData.size()), record->personData.begin());
            }
            const SavedNeighbor* neighbor = world.neighbor(id);
            std::vector<std::uint32_t> kept, added;
            for (const std::int16_t to : world.neighborRelationshipTargets(id)) {
                if (isLeftOut(to)) {
                    continue;
                }
                kept.push_back(static_cast<std::uint16_t>(to));
            }
            // Made since the neighborhood loaded, with a record already saved or saved now.
            const std::vector<std::int16_t>& fresh = made[id];
            for (const auto& [from, to] : world.neighborRelationshipsAdded()) {
                if (from == id && (relationshipRecords.contains(instanceOf(from, to)) ||
                                   std::find(fresh.begin(), fresh.end(), to) != fresh.end())) {
                    added.push_back(static_cast<std::uint16_t>(to));
                }
            }
            record->relationships = savedRelationshipList(neighbor ? std::span<const std::uint32_t>(neighbor->relationshipTable)
                                                                   : std::span<const std::uint32_t>(record->relationships),
                                                          kept, added);
            resource = dbpf::storeResource(resource.key, sdsc::writeRecord(*record));
            ++counts.descriptions;
            for (const std::int16_t to : fresh) {
                const dbpf::ResourceKey key{srel::kType, 0xFFFFFFFF, instanceOf(id, to), 0};
                madeRecords.push_back(dbpf::storeResource(key, srel::writeRecord({*world.neighborRelationshipValues(id, to)})));
            }
        } else if (resource.key.type == fami::kType) {
            fami::Family family;
            try {
                family = fami::parse(bodyOf(resource));
            } catch (const std::runtime_error&) {
                continue;
            }
            if (family.version != fami::kWrittenVersion || family.extraBytes != 0) {
                continue;
            }
            bool rewrite = false;
            // The family living on the lot saves the lot's funds, and they become its last saved funds.
            if (household && resource.key.instance == *household && globals.size() > global::kBudgetHigh) {
                family.funds = funds(globals);
                family.funds2 = family.funds;
                rewrite = true;
                counts.family = true;
            }
            // A Sim born or made into the family joins its members, after those it had.
            std::vector<std::uint32_t> members;
            for (const std::uint32_t guid : world.familyMemberGuids(resource.key.instance)) {
                if (!leftOutGuids.contains(guid)) {
                    members.push_back(guid);
                }
            }
            if (world.hasFamily(resource.key.instance) && members != family.members) {
                family.members = std::move(members);
                rewrite = true;
                ++counts.familyMembers;
            }
            if (rewrite) {
                resource = dbpf::storeResource(resource.key, fami::write(family));
            }
        } else if (resource.key.type == famt::kType) {
            counts.familyTies = hoodsave::saveFamilyTies(resource, world, leaveOut ? world.addedNeighbors() : std::set<std::int16_t>{});
        } else if (resource.key.type == ngbh::kType) {
            counts.inventories = hoodsave::saveInventories(resource, world, leaveOut ? world.addedNeighbors() : std::set<std::int16_t>{});
        } else if (resource.key.type == lotrecords::kShortRecordType && !leaveOut) {
            counts.neighborCounter = hoodsave::saveNeighborCounter(resource, world);
        } else if (resource.key.type == nhood::kLotDescriptionType && resource.key.instance == lotId && lotInfo) {
            std::optional<nhood::LotDescriptionRecord> record = nhood::readLotDescriptionRecord(bodyOf(resource));
            if (!record) {
                continue;
            }
            record->own = *lotInfo;
            resource = dbpf::storeResource(resource.key, nhood::writeLotDescriptionRecord(*record));
            counts.lotDescription = true;
        }
    }
    for (dbpf::StoredResource& resource : madeRecords) {
        hood.resources.push_back(std::move(resource));
        hood.dataOrder.push_back(hood.resources.size() - 1);
        ++counts.relationships;
    }
    return counts;
}

} // namespace ts2::sim
