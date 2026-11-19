#include "sim/hood_save_detail.hpp"

#include "formats/famt.hpp"
#include "formats/lot_records.hpp"
#include "formats/ngbh.hpp"
#include "formats/qfs.hpp"
#include "formats/resource_types.hpp"
#include "formats/sdsc.hpp"
#include "formats/swaf.hpp"
#include "sim/world.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace ts2::sim::hoodsave {

namespace {

constexpr std::uint32_t kRecessiveKey = 0x10000000;  // a gene's recessive allele is saved under its number plus this

bool hasResource(const dbpf::PackageImage& hood, const dbpf::ResourceKey& key) {
    return std::any_of(hood.resources.begin(), hood.resources.end(),
                       [&](const dbpf::StoredResource& r) { return r.key == key; });
}

void append(dbpf::PackageImage& hood, const dbpf::ResourceKey& key, std::span<const std::uint8_t> body) {
    hood.resources.push_back(dbpf::storeResource(key, body));
    hood.dataOrder.push_back(hood.resources.size() - 1);
}

} // namespace

std::vector<std::uint8_t> bodyOf(const dbpf::StoredResource& resource) {
    return resource.compressed ? qfs::decompress(resource.bytes) : resource.bytes;
}

std::optional<std::vector<propset::Property>> dnaProperties(const Genome& genome) {
    static constexpr std::array<std::uint32_t, 5> kOrder{kSkinGene, kSkinAllelesGene, kHairGene, kEyeGene, kFaceGene};
    for (const auto& [gene, value] : genome) {
        if (std::find(kOrder.begin(), kOrder.end(), gene) == kOrder.end()) {
            return std::nullopt;
        }
    }
    std::vector<propset::Property> out;
    for (const std::uint32_t gene : kOrder) {
        const auto it = genome.find(gene);
        if (it == genome.end()) {
            continue;
        }
        out.push_back({propset::kStringType, std::to_string(gene), 0, it->second.dominant, {}});
        out.push_back({propset::kStringType, std::to_string(gene + kRecessiveKey), 0, it->second.recessive, {}});
    }
    return out;
}

int addNewDescriptions(dbpf::PackageImage& hood, const World& world) {
    int added = 0;
    for (const std::int16_t id : world.addedNeighbors()) {
        const SavedNeighbor* neighbor = world.neighbor(id);
        const dbpf::ResourceKey key{sdsc::kType, 0xFFFFFFFF, static_cast<std::uint32_t>(static_cast<std::uint16_t>(id)), 0};
        if (neighbor == nullptr || hasResource(hood, key)) {
            continue;
        }
        sdsc::Record record;
        for (std::size_t i = 0; i < record.personData.size(); ++i) {
            record.personData[i] = world.neighborPersonValue(id, i);
        }
        record.neighborId = id;
        record.guid = neighbor->guid;
        append(hood, key, sdsc::writeRecord(record));
        ++added;
    }
    return added;
}

int addNewDna(dbpf::PackageImage& hood, const World& world) {
    std::vector<std::uint32_t> made;
    for (const std::int16_t id : world.addedNeighbors()) {
        const SavedNeighbor* neighbor = world.neighbor(id);
        const auto instance = static_cast<std::uint32_t>(static_cast<std::uint16_t>(id));
        const dbpf::ResourceKey key{types::kSimDna, 0xFFFFFFFF, instance, 0};
        if (neighbor == nullptr || neighbor->genes.empty() || hasResource(hood, key)) {
            continue;
        }
        const std::optional<std::vector<propset::Property>> properties = dnaProperties(neighbor->genes);
        if (!properties) {
            continue;
        }
        append(hood, key, propset::writeXml(*properties));
        made.push_back(instance);
    }
    if (made.empty()) {
        return 0;
    }
    for (dbpf::StoredResource& resource : hood.resources) {
        if (resource.key.type != lotrecords::kDnaIndexType) {
            continue;
        }
        std::optional<std::vector<std::uint32_t>> index = lotrecords::readDnaIndex(bodyOf(resource));
        if (!index) {
            break;
        }
        for (const std::uint32_t instance : made) {
            const auto at = std::lower_bound(index->begin(), index->end(), instance);
            if (at == index->end() || *at != instance) {
                index->insert(at, instance);
            }
        }
        resource = dbpf::storeResource(resource.key, lotrecords::writeDnaIndex(*index));
        break;
    }
    return static_cast<int>(made.size());
}

bool saveFamilyTies(dbpf::StoredResource& resource, const World& world, const std::set<std::int16_t>& leftOut) {
    const std::map<std::int16_t, std::vector<famt::Tie>>* now = world.allFamilyTies();
    if (now == nullptr) {
        return false;
    }
    famt::FamilyTies record;
    try {
        record = famt::parse(bodyOf(resource));
    } catch (const std::runtime_error&) {
        return false;
    }
    if (record.version != famt::kWrittenVersion || record.extraBytes != 0) {
        return false;
    }
    const auto same = [](const std::vector<famt::Tie>& a, const std::vector<famt::Tie>& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [](const famt::Tie& x, const famt::Tie& y) { return x.type == y.type && x.other == y.other; });
    };
    const auto kept = [&](const std::vector<famt::Tie>& ties) {
        std::vector<famt::Tie> out;
        for (const famt::Tie& tie : ties) {
            if (!leftOut.contains(tie.other)) {
                out.push_back(tie);
            }
        }
        return out;
    };
    bool changed = false;
    std::set<std::int16_t> listed;
    for (famt::SimTies& sim : record.sims) {
        listed.insert(sim.sim);
        if (const auto it = now->find(sim.sim); it != now->end()) {
            std::vector<famt::Tie> ties = kept(it->second);
            if (!same(ties, sim.ties)) {
                sim.ties = std::move(ties);
                changed = true;
            }
        }
    }
    for (const auto& [sim, all] : *now) {
        const std::vector<famt::Tie> ties = kept(all);
        if (!listed.contains(sim) && !leftOut.contains(sim) && !ties.empty()) {
            const auto at = std::lower_bound(record.sims.begin(), record.sims.end(), sim,
                                             [](const famt::SimTies& s, std::int16_t id) { return s.sim < id; });
            record.sims.insert(at, famt::SimTies{sim, 1, ties});
            changed = true;
        }
    }
    if (changed) {
        resource = dbpf::storeResource(resource.key, famt::write(record));
    }
    return changed;
}

bool saveNeighborCounter(dbpf::StoredResource& resource, const World& world) {
    const std::optional<std::uint16_t> saved = lotrecords::readShortRecord(bodyOf(resource));
    const auto now = static_cast<std::uint16_t>(world.nextNeighborId());
    if (!saved || *saved == now) {
        return false;
    }
    resource = dbpf::storeResource(resource.key, lotrecords::writeShortRecord(now));
    return true;
}

namespace {

std::vector<xobj::Token> savedTokens(const std::vector<std::shared_ptr<InventoryToken>>& tokens) {
    std::vector<xobj::Token> out;
    out.reserve(tokens.size());
    for (const auto& t : tokens) {
        if (t) {
            out.push_back({t->guid, t->flags, t->value1, t->value2, t->category, t->properties});
        }
    }
    return out;
}

// Puts `inventory` in at `id`, keeping the list in ascending id order.
void putInventory(std::vector<std::pair<std::int16_t, ngbh::Inventory>>& list, std::int16_t id, const Inventory& inventory) {
    auto at = std::lower_bound(list.begin(), list.end(), id, [](const auto& entry, std::int16_t key) { return entry.first < key; });
    if (at == list.end() || at->first != id) {
        at = list.insert(at, {id, ngbh::Inventory{ngbh::kInventoryVersion, {}, {}}});
    }
    at->second.counted = savedTokens(inventory.counted);
    at->second.singular = savedTokens(inventory.singular);
}

} // namespace

int saveInventories(dbpf::StoredResource& resource, const World& world, const std::set<std::int16_t>& leftOut) {
    std::optional<ngbh::Record> record = ngbh::readRecord(bodyOf(resource));
    if (!record) {
        return 0;
    }
    int written = 0;
    for (const auto& [key, inventory] : world.usedInventories()) {
        const auto [kind, id] = key;
        if (kind == InventoryKind::Global) {
            record->global.counted = savedTokens(inventory.counted);
            record->global.singular = savedTokens(inventory.singular);
            ++written;
            continue;
        }
        // Keys are 16-bit in the record. The game-wide inventory isn't saved with the neighborhood.
        if (kind == InventoryKind::GameWide || id <= 0 || id > std::numeric_limits<std::int16_t>::max()) {
            continue;
        }
        const auto key16 = static_cast<std::int16_t>(id);
        if (kind == InventoryKind::Neighbor && leftOut.contains(key16)) {
            continue;
        }
        auto& list = kind == InventoryKind::Lot ? record->lots : kind == InventoryKind::Family ? record->families : record->neighbors;
        putInventory(list, key16, inventory);
        ++written;
    }
    if (written > 0) {
        resource = dbpf::storeResource(resource.key, ngbh::writeRecord(*record));
    }
    return written;
}

namespace {

// The wants and fears of a list that fill a place, in place order.
std::vector<swaf::Want> filledPlaces(std::vector<swaf::Want> list) {
    std::erase_if(list, [](const swaf::Want& w) { return w.guid == 0; });
    return list;
}

} // namespace

int saveWants(dbpf::PackageImage& hood, const World& world, const std::set<std::int16_t>& leftOut) {
    std::unordered_map<std::uint32_t, std::size_t> records;
    for (std::size_t i = 0; i < hood.resources.size(); ++i) {
        if (hood.resources[i].key.type == swaf::kType) {
            records.emplace(hood.resources[i].key.instance, i);
        }
    }
    int written = 0;
    for (const auto& [id, history] : world.wantsState().histories) {
        if (id <= 0 || leftOut.contains(id)) {
            continue;
        }
        const auto instance = static_cast<std::uint32_t>(id);
        const auto at = records.find(instance);
        std::vector<std::uint8_t> old;
        std::optional<swaf::Record> loaded;
        if (at != records.end()) {
            old = bodyOf(hood.resources[at->second]);
            std::size_t leftOver = 0;
            loaded = swaf::readRecord(old, &leftOver);
            if (!loaded) {
                continue;
            }
        }
        swaf::Record record;
        const swaf::Slots slots = world.wantSlots(id);
        record.extra = loaded ? loaded->extra : std::nullopt;
        record.maxWants = loaded ? loaded->maxWants : slots.wants;
        record.maxFears = loaded ? loaded->maxFears : slots.fears;
        record.lastValue = loaded ? loaded->lastValue : 0;
        record.wants = filledPlaces(world.wantsNow(id, false));
        record.fears = filledPlaces(world.wantsNow(id, true));
        record.historyValue = history.time;
        record.history.assign(history.records.begin(), history.records.end());
        std::vector<std::uint8_t> body;
        try {
            body = swaf::writeRecord(record);
        } catch (const std::runtime_error&) {
            // A want loaded from an older record lacks the numbers the game writes now.
            continue;
        }
        if (old.size() > body.size()) {
            body.insert(body.end(), old.begin() + static_cast<std::ptrdiff_t>(body.size()), old.end());
        }
        const dbpf::ResourceKey key{swaf::kType, 0xFFFFFFFF, instance, 0};
        if (at != records.end()) {
            hood.resources[at->second] = dbpf::storeResource(key, body);
        } else {
            append(hood, key, body);
        }
        ++written;
    }
    return written;
}

} // namespace ts2::sim::hoodsave
