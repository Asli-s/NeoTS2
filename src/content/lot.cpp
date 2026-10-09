#include "content/lot.hpp"

#include "formats/dbpf.hpp"
#include "formats/resource_types.hpp"

#include <algorithm>
#include <exception>
#include <map>

namespace ts2::content {

namespace {

constexpr std::size_t kObjectIdField = 0x0B;
constexpr std::uint32_t kRoomGraphInstance = 5;

xobj::RecordKind recordKind(std::uint32_t resourceType) {
    return resourceType == types::kSavedPerson               ? xobj::RecordKind::Person
           : resourceType == types::kSavedObjectWithMaterial ? xobj::RecordKind::ObjectWithMaterial
                                                             : xobj::RecordKind::Object;
}

} // namespace

Lot loadLot(const std::filesystem::path& path) {
    dbpf::Package package{path};
    Lot lot;
    bool haveTypes = false;
    bool haveMap = false;
    std::map<std::uint32_t, std::vector<const dbpf::IndexEntry*>> records;
    for (const auto& e : package.entries()) {
        if (e.key.type == types::kObjectTypeTable) {
            lot.types = objt::parse(package.read(e));
            haveTypes = true;
        } else if (e.key.type == types::kObjectMap && e.key.instance == 1) {
            lot.objectMap = objm::parse(package.read(e));
            haveMap = true;
        } else if (e.key.type == types::kWallGraph && e.key.instance == kRoomGraphInstance) {
            lot.roomGraph = wallgraph::parse(package.read(e));
        } else if (e.key.type == types::kWallGraph &&
                   std::ranges::find(kLayerGraphInstances, e.key.instance) != std::end(kLayerGraphInstances)) {
            lot.layerGraphs.insert_or_assign(e.key.instance, wallgraph::parse(package.read(e)));
        } else if (e.key.type == types::kDataTables) {
            try {
                lot.dataTables.emplace(e.key.instance, datatables::parse(package.read(e)));
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("data tables: ") + ex.what()});
            }
        } else if (e.key.type == types::kArray3D) {
            try {
                lot.grids.emplace(e.key.instance, array3d::parse(package.read(e)));
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("grid: ") + ex.what()});
            }
        } else if (e.key.type == types::kPatternMap &&
                   (e.key.instance == kFloorPatternMapInstance || e.key.instance == kWallPatternMapInstance)) {
            try {
                (e.key.instance == kFloorPatternMapInstance ? lot.floorPatterns : lot.wallPatterns) =
                    stringmap::parse(package.read(e));
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("pattern map: ") + ex.what()});
            }
        } else if (e.key.type == types::kWallLayer && e.key.instance == kWallCoveringLayerInstance) {
            try {
                lot.wallCoverings = walllayer::parse(package.read(e));
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("wall layer: ") + ex.what()});
            }
        } else if (e.key.type == types::kFencePostLayer) {
            try {
                lot.fencePosts = fencepostlayer::parse(package.read(e));
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("fence posts: ") + ex.what()});
            }
        } else if (e.key.type == types::kRoofs) {
            try {
                lot.roofs = roofs::parse(package.read(e));
            } catch (const std::exception& ex) {
                lot.problems.push_back({e.key.instance, std::string("roofs: ") + ex.what()});
            }
        } else if (e.key.type == types::kSavedObject || e.key.type == types::kSavedObjectWithMaterial ||
                   e.key.type == types::kSavedPerson) {
            records[e.key.instance].push_back(&e);
        }
    }
    if (!haveTypes && !haveMap && records.empty()) {
        return lot;
    }
    if (!haveTypes || !haveMap) {
        throw std::runtime_error("lot: missing object type table or object map");
    }

    for (const auto& [id, candidates] : records) {
        const auto listed = lot.objectMap.typeIndexByObject.find(id);
        if (listed == lot.objectMap.typeIndexByObject.end()) {
            lot.staleRecords += candidates.size();
            continue;
        }
        const auto* type = lot.types.find(static_cast<std::uint16_t>(listed->second));
        if (!type) {
            lot.problems.push_back({id, "object map points at a missing type"});
            lot.staleRecords += candidates.size();
            continue;
        }
        const std::uint32_t wanted = type->kind == objt::kPersonKind ? types::kSavedPerson
                                     : (type->flag & 1)              ? types::kSavedObjectWithMaterial
                                                                     : types::kSavedObject;
        std::optional<LotObject> chosen;
        for (const auto* e : candidates) {
            if (e->key.type != wanted) {
                ++lot.staleRecords;
                continue;
            }
            std::optional<xobj::SavedObject> state;
            try {
                state = xobj::parse(package.read(*e), lot.objectMap.version, recordKind(e->key.type));
            } catch (const std::exception& ex) {
                lot.problems.push_back({id, ex.what()});
                continue;
            }
            if (static_cast<std::uint16_t>(state->data[kObjectIdField]) != id) {
                lot.problems.push_back({id, "object data field 0x0B is not the object's id"});
            }
            chosen = LotObject{id, e->key.type, *type, std::move(*state)};
        }
        if (!chosen) {
            lot.problems.push_back({id, "no record of the type the object table asks for"});
            continue;
        }
        lot.objects.push_back(std::move(*chosen));
    }
    for (const auto& [id, typeIndex] : lot.objectMap.typeIndexByObject) {
        if (!records.contains(id)) {
            lot.problems.push_back({id, "listed object has no saved record"});
        }
    }
    return lot;
}

} // namespace ts2::content
