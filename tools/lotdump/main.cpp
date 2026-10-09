#include "content/install.hpp"
#include "content/lot.hpp"
#include "content/resource_index.hpp"
#include "content/neighborhood.hpp"
#include "formats/dbpf.hpp"
#include "formats/fami.hpp"
#include "formats/famt.hpp"
#include "formats/sdsc.hpp"
#include "formats/slot.hpp"
#include "formats/wallgraph.hpp"
#include "formats/resource_types.hpp"
#include "sim/index_code_source.hpp"
#include "sim/rooms.hpp"
#include "sim/world.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <set>
#include <span>
#include <string_view>

namespace {

using ts2::content::Lot;
using ts2::content::LotObject;
using ts2::sim::IndexCodeSource;

constexpr std::size_t kRoomField = 0x1D;

const ts2::sim::ObjectType* definitionOf(const LotObject& o, IndexCodeSource* code) {
    return code && o.type ? code->objectType(o.type->guid) : nullptr;
}

void printShorts(const char* label, std::span<const std::int16_t> values) {
    std::printf("  %-10s", label);
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::printf("%s%d", i % 16 == 0 && i ? "\n            " : " ", values[i]);
    }
    std::printf("\n");
}

int showObject(const std::filesystem::path& path, std::uint32_t id) {
    const Lot lot = ts2::content::loadLot(path);
    for (const auto& o : lot.objects) {
        if (o.id != id) {
            continue;
        }
        const auto& s = o.state;
        std::printf("object %u: %s, type %s 0x%08X\n", o.id, ts2::types::typeName(o.resourceType).c_str(),
                    o.type ? o.type->name.c_str() : "-", o.type ? o.type->guid : 0);
        printShorts("attributes", s.attributes);
        printShorts("semi", s.semiAttributes);
        printShorts("temps", s.temps);
        printShorts("data", s.data);
        if (s.tail) {
            for (const auto& f : s.tail->stack) {
                std::printf("  frame: object %d, tree 0x%04X node %u, values %d %d %d, owner %d\n",
                            f.objectId, static_cast<std::uint16_t>(f.treeId), f.node, f.value1, f.value2,
                            f.value3, f.codeOwner);
                printShorts("  params", f.params);
                printShorts("  locals", f.locals);
            }
        }
        if (s.person) {
            std::printf("  motives  ");
            for (const float m : s.person->motives) {
                std::printf(" %.1f", m);
            }
            std::printf("\n");
            printShorts("person", s.person->personData);
        }
        return 0;
    }
    std::fprintf(stderr, "object %u is not on the lot\n", id);
    return 1;
}

int listLot(const std::filesystem::path& path, IndexCodeSource* code) {
    const Lot lot = ts2::content::loadLot(path);
    std::printf("version 0x%X, object types %zu, objects %zu, stale records %zu, problems %zu\n",
                lot.objectMap.version, lot.types.entries.size(), lot.objects.size(),
                lot.staleRecords, lot.problems.size());
    std::printf("%5s  %-6s %4s %4s  %-10s %-10s  %s\n", "id", "res", "attr", "semi", "guid", "group",
                "name");
    for (const auto& o : lot.objects) {
        const auto* definition = definitionOf(o, code);
        std::printf("%5u  %-6s %4zu %4zu  0x%08X ", o.id, ts2::types::typeName(o.resourceType).c_str(),
                    o.state.attributes.size(), o.state.semiAttributes.size(), o.type ? o.type->guid : 0);
        if (definition) {
            std::printf("0x%08X  %s%s\n", definition->group, definition->definition.name.c_str(),
                        definition->definition.field(ts2::objd::field::kType) == ts2::sim::kPersonType
                            ? " [person]"
                            : "");
        } else {
            std::printf("%-10s  %s\n", "-", o.type ? o.type->name.c_str() : "");
        }
    }
    for (const auto& p : lot.problems) {
        std::printf("problem: object %u: %s\n", p.id, p.message.c_str());
    }
    return 0;
}

// Rebuilds regions from every wall graph in the lot and shows, for each saved room (grid 9),
// which region of each graph covers most of its quarter tiles.
int showGraphs(const std::filesystem::path& path) {
    const Lot lot = ts2::content::loadLot(path);
    const auto grid = lot.grids.find(9);
    if (grid == lot.grids.end() || !lot.roomGraph) {
        std::fprintf(stderr, "the lot has no saved room grid or room graph\n");
        return 1;
    }
    const ts2::sim::RoomMap saved(grid->second, lot.roomGraph->firstLevel);
    ts2::dbpf::Package package{path};
    for (const auto& e : package.entries()) {
        if (e.key.type != ts2::types::kWallGraph) {
            continue;
        }
        const auto graph = ts2::wallgraph::parse(package.read(e));
        const ts2::sim::RoomMap regions(graph);
        std::printf("wall graph 0x%X: %zu points, %zu walls, rooms", e.key.instance, graph.points.size(),
                    graph.walls.size());
        for (const std::int32_t id : graph.rooms) {
            std::printf(" %d", id);
        }
        std::printf("\n");
        std::map<std::int32_t, std::map<std::int32_t, std::int32_t>> overlap;  // saved room -> region -> quarters
        for (std::int32_t level = saved.firstLevel(); level < saved.firstLevel() + saved.levelCount(); ++level) {
            for (std::int32_t y = 0; y < saved.height(); ++y) {
                for (std::int32_t x = 0; x < saved.width(); ++x) {
                    for (int q = 0; q < 4; ++q) {
                        const std::int32_t room = saved.quarterRoom(level, x, y, q);
                        if (room != ts2::sim::RoomMap::kNoRoom) {
                            ++overlap[room][regions.quarterRoom(level, x, y, q)];
                        }
                    }
                }
            }
        }
        for (const auto& [room, byRegion] : overlap) {
            const auto best = std::max_element(byRegion.begin(), byRegion.end(),
                                               [](const auto& a, const auto& b) { return a.second < b.second; });
            std::int32_t total = 0;
            for (const auto& [region, n] : byRegion) {
                total += n;
            }
            const auto* info = saved.room(room);
            std::printf("  saved room %3d -> region %3d (%d of %d quarters), level %d\n", room, best->first,
                        best->second, total, info ? info->level : 0);
        }
    }
    return 0;
}

// Lists a neighborhood's neighbors (Sim descriptions) with the name of the object type their GUID
// names (character files, or the install for townies and NPCs), or one neighbor's person data.
int showNeighbors(const std::filesystem::path& hood, IndexCodeSource& code, std::optional<std::int16_t> only) {
    ts2::dbpf::Package package{ts2::content::neighborhoodPackage(hood)};
    std::size_t total = 0;
    std::size_t idMatches = 0;
    std::size_t named = 0;
    for (const auto& e : package.entries()) {
        if (e.key.type != ts2::types::kSimDescription) {
            continue;
        }
        ++total;
        const auto s = ts2::sdsc::parse(package.read(e));
        idMatches += static_cast<std::uint32_t>(s.neighborId) == e.key.instance ? 1 : 0;
        const auto* type = code.objectType(s.guid);
        named += type ? 1 : 0;
        if (only && *only != s.neighborId) {
            continue;
        }
        std::printf("neighbor %4d  guid 0x%08X  version 0x%X  person data v0x%X: %3zu values  rest %3zu bytes  %s\n",
                    s.neighborId, s.guid, s.version, static_cast<unsigned>(s.personDataVersion),
                    s.personData.size(), s.rest.size(), type ? type->definition.name.c_str() : "-");
        if (only) {
            printShorts("person", s.personData);
        }
    }
    if (!only) {
        std::printf("%zu neighbors; id equals instance for %zu; %zu GUIDs name an object type\n", total, idMatches,
                    named);
    }
    return 0;
}

// Summarizes every neighbor's saved wants, or lists one neighbor's wants, fears and history.
int showWants(const std::filesystem::path& hood, std::optional<std::int16_t> only) {
    const auto n = ts2::content::loadNeighborhood(ts2::content::neighborhoodPackage(hood));
    auto printWant = [](const char* kind, const ts2::swaf::Want& w) {
        std::printf("  %-7s v%-2u id %u  guid 0x%08X  type %u  argument %d  value %d  tail", kind, w.version, w.id,
                    w.guid, w.type, w.argument, w.value);
        for (const auto t : w.tail) {
            std::printf(" %d", t);
        }
        std::printf("  flags %u\n", w.flags);
    };
    if (only) {
        const auto it = n.wants.find(*only);
        if (it == n.wants.end()) {
            std::printf("no saved wants for neighbor %d\n", *only);
            return 1;
        }
        const auto& s = it->second;
        std::printf("neighbor %d: version %u, max wants %u, max fears %u, history v%u (%d), %zu extra bytes\n", *only,
                    s.version, s.maxWants, s.maxFears, s.historyVersion, s.historyValue, s.extraBytes);
        if (s.extra) {
            printWant("extra", *s.extra);
        }
        for (const auto& w : s.wants) {
            printWant("want", w);
        }
        for (const auto& w : s.fears) {
            printWant("fear", w);
        }
        std::size_t done = 0;
        for (const auto& [guid, list] : s.history) {
            done += list.size();
        }
        std::printf("  history: %zu want types, %zu entries\n", s.history.size(), done);
        return 0;
    }
    std::map<std::uint32_t, std::size_t> versions;
    std::size_t wants = 0, fears = 0, history = 0, withoutNeighbor = 0;
    for (const auto& [id, s] : n.wants) {
        ++versions[s.version];
        wants += s.wants.size();
        fears += s.fears.size();
        for (const auto& [guid, list] : s.history) {
            history += list.size();
        }
        withoutNeighbor += n.neighborIds.contains(id) ? 0 : 1;
    }
    std::printf("%zu wants records (%zu with stale bytes after the history, %zu for ids that are not neighbors)\n",
                n.wants.size(), n.wantsWithExtraBytes, withoutNeighbor);
    for (const auto& [v, count] : versions) {
        std::printf("  version %u: %zu\n", v, count);
    }
    std::printf("  %zu wants, %zu fears, %zu history entries\n", wants, fears, history);
    for (const auto& p : n.problems) {
        std::printf("  problem: %s\n", p.c_str());
    }
    return 0;
}

// Lists a neighborhood's families and checks them: does their lot exist, are their members neighbors.
int showFamilies(const std::filesystem::path& hood) {
    ts2::dbpf::Package package{ts2::content::neighborhoodPackage(hood)};
    std::set<std::uint32_t> neighborGuids;
    for (const auto& e : package.entries()) {
        if (e.key.type == ts2::types::kSimDescription) {
            neighborGuids.insert(ts2::sdsc::parse(package.read(e)).guid);
        }
    }
    std::size_t families = 0, lotsFound = 0, withLot = 0, members = 0, membersFound = 0, inexact = 0;
    for (const auto& e : package.entries()) {
        if (e.key.type != ts2::types::kFamily) {
            continue;
        }
        ++families;
        const auto f = ts2::fami::parse(package.read(e));
        inexact += f.extraBytes != 0 ? 1 : 0;
        const auto lotFile = hood / "Lots" /
                             (hood.filename().string() + "_Lot" + std::to_string(f.lot) + ".package");
        const bool lotExists = f.lot != 0 && std::filesystem::exists(lotFile);
        withLot += f.lot != 0 ? 1 : 0;
        lotsFound += lotExists ? 1 : 0;
        std::size_t found = 0;
        for (const std::uint32_t g : f.members) {
            found += neighborGuids.contains(g) ? 1 : 0;
        }
        members += f.members.size();
        membersFound += found;
        std::printf("family 0x%04X  v0x%X  lot %4d%s  lot2 %4d  funds %7d / %7d  flags 0x%X  values %d %d %d %u  "
                    "id 0x%08X  members %zu (%zu neighbors)\n",
                    e.key.instance, f.version, f.lot, f.lot == 0 ? "" : lotExists ? "" : "?", f.lot2, f.funds,
                    f.funds2, f.flags, f.value3, f.value4, f.value6, f.value8, f.id7, f.members.size(), found);
    }
    std::printf("%zu families (%zu with bytes left over); %zu of %zu lots exist; %zu of %zu members are "
                "neighbors\n",
                families, inexact, lotsFound, withLot, membersFound, members);
    for (const auto& e : package.entries()) {
        if (e.key.type != ts2::types::kFamilyTies) {
            continue;
        }
        const auto f = ts2::famt::parse(package.read(e));
        std::map<std::uint32_t, std::size_t> byType;
        for (const auto& s : f.sims) {
            for (const auto& t : s.ties) {
                ++byType[t.type];
            }
        }
        const char* names[] = {"father", "mother", "spouse", "sibling", "child"};
        std::printf("family ties v%u: %zu Sims, %zu bytes left over;", f.version, f.sims.size(), f.extraBytes);
        for (const auto& [type, n] : byType) {
            std::printf(" %s %zu", type < 5 ? names[type] : "?", n);
        }
        std::printf("\n");
    }
    return 0;
}

// Compares a saved stack frame with the tree it names; the tree id picks the global, private or
// semi-global group of the code owner.
std::string checkFrame(const Lot& lot, const ts2::xobj::StackFrame& f, IndexCodeSource& code) {
    const auto* owner = lot.types.find(static_cast<std::uint16_t>(f.codeOwner));
    const auto* ownerType = owner ? code.objectType(owner->guid) : nullptr;
    if (!ownerType) {
        return "code owner not found";
    }
    const auto tree = static_cast<std::uint16_t>(f.treeId);
    const std::uint32_t group = tree < 0x1000   ? ts2::sim::kGlobalGroup
                                : tree < 0x2000 ? ownerType->group
                                                : code.semiGlobalGroup(ownerType->group);
    const auto bhav = code.behavior(group, tree);
    if (!bhav) {
        return "tree not found";
    }
    if (f.node >= bhav->instructions.size()) {
        return "node beyond the tree";
    }
    if (f.params.size() != bhav->argCount || f.locals.size() != bhav->localCount) {
        return "param or local count differs from the tree";
    }
    return "matches its tree";
}

int survey(const std::filesystem::path& root, IndexCodeSource* code, ts2::content::ResourceIndex* index) {
    std::size_t lots = 0;
    std::size_t failed = 0;
    std::size_t objects = 0;
    std::size_t stale = 0;
    std::size_t withDefinition = 0;
    std::size_t persons = 0;
    std::size_t decodedTails = 0;
    std::size_t decodedSims = 0;
    std::size_t frames = 0;
    std::map<std::string, std::size_t> frameChecks;
    std::map<std::string, std::size_t> roomChecks;
    std::map<std::uint32_t, std::size_t> versions;
    std::map<std::string, std::size_t> problems;
    std::map<std::size_t, std::size_t> arrayCounts;                  // object arrays per saved object
    std::map<std::uint32_t, std::set<std::size_t>> arrayCountsByType;  // by GUID
    std::map<std::size_t, std::size_t> fieldHits;  // OBJD field -> objects with arrays where it equals the count
    std::size_t withArraysDefined = 0;
    std::map<std::string, std::size_t> field59;  // OBJD field 59 against the saved array count
    std::size_t iteratorsMatch[2] = {};          // the counted shorts after the arrays: one per array?
    std::map<std::string, std::size_t> slotMarkers;
    std::map<std::string, std::size_t> slotChecks;  // saved slot values against the objects they name
    std::map<std::string, std::size_t> multiTile;   // multi-tile groups by lead object
    std::map<std::string, std::size_t> slotCounts;  // saved slot count against the SLOT resource
    std::map<std::string, std::size_t> slotOdd;     // the cases that differ, in detail
    std::map<std::uint32_t, std::size_t> arrayExamples;  // object group -> array count
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".package" ||
            entry.path().parent_path().filename() != "Lots") {
            continue;
        }
        ++lots;
        try {
            const Lot lot = ts2::content::loadLot(entry.path());
            ++versions[lot.objectMap.version];
            objects += lot.objects.size();
            stale += lot.staleRecords;
            // Multi-tile objects: parts share object data 0x13 ("lead tile object ID").
            {
                constexpr std::size_t kLeadField = 0x13;
                constexpr std::size_t kMasterIdField = 0x0A;
                constexpr std::size_t kSubIndexField = 0x0B;
                std::map<std::int16_t, std::vector<std::pair<std::uint32_t, std::int16_t>>> groups;  // lead -> (id, sub index)
                for (const auto& o : lot.objects) {
                    const std::int16_t lead = o.state.data[kLeadField];
                    const auto* d = definitionOf(o, code);
                    const std::int16_t master = d ? static_cast<std::int16_t>(d->definition.field(kMasterIdField)) : -1;
                    if (lead == 0) {
                        ++multiTile[master > 0 ? "no lead, OBJD master id set" : "no lead"];
                        continue;
                    }
                    ++multiTile[master > 0 ? "lead set, OBJD master id set" : master == 0 ? "lead set, OBJD master id 0" : "lead set, no definition"];
                    groups[lead].push_back({o.id, d ? static_cast<std::int16_t>(d->definition.field(kSubIndexField)) : std::int16_t{0}});
                }
                for (const auto& [lead, parts] : groups) {
                    ++multiTile[std::any_of(parts.begin(), parts.end(), [lead](const auto& p) { return p.first == static_cast<std::uint32_t>(lead); })
                                    ? "group: lead is a member" : "group: lead not a member"];
                    ++multiTile[parts.size() == 1 ? "group: one part" : "group: several parts"];
                    if (parts.size() > 1) {
                        auto byId = parts;
                        std::sort(byId.begin(), byId.end());
                        bool subAscending = true;
                        for (std::size_t i = 1; i < byId.size(); ++i) {
                            subAscending &= byId[i - 1].second < byId[i].second;
                        }
                        ++multiTile[subAscending ? "group: id order = sub index order" : "group: id order differs from sub index order"];
                    }
                }
            }
            auto checkRooms = [&](const ts2::sim::RoomMap& rooms, const std::string& source) {
                for (const auto& o : lot.objects) {
                    const auto& h = o.state.header;
                    if (h.location[0] < 0 || h.location[1] < 0) {
                        continue;
                    }
                    const std::int32_t room = rooms.roomAt(h.value1 - 1, h.location[1], h.location[0]);
                    ++roomChecks[source + (room == o.state.data[kRoomField]   ? ": matches the object's saved room"
                                           : room == ts2::sim::RoomMap::kNoRoom ? ": no room"
                                                                                : ": differs from the object's saved room")];
                }
            };
            if (const auto saved = lot.grids.find(ts2::content::kRoomGridInstance); saved != lot.grids.end()) {
                checkRooms(ts2::sim::RoomMap(saved->second, lot.roomGraph ? lot.roomGraph->firstLevel : 0),
                           "saved room grid");
            }
            if (lot.roomGraph) {
                checkRooms(ts2::sim::RoomMap(*lot.roomGraph), "rooms rebuilt from walls");
            }
            for (const auto& o : lot.objects) {
                if (code && o.state.tail) {
                    for (const auto& f : o.state.tail->stack) {
                        ++frameChecks[checkFrame(lot, f, *code)];
                    }
                }
                if (o.state.tail) {
                    const auto& s = o.state.tail->slots;
                    if (const auto* d = definitionOf(o, code); d && index && s.marker < 0) {
                        const auto slotsId = d->definition.field(ts2::objd::field::kSlotsId);
                        const auto* r = index->find({ts2::types::kSlots, d->group, slotsId, 0});
                        std::optional<std::vector<std::uint16_t>> types;
                        if (r) {
                            try {
                                types = ts2::slot::parse(index->read(*r)).types;
                            } catch (const std::exception&) {
                            }
                        }
                        if (!types) {
                            ++slotCounts[slotsId == 0 ? "no SLOT id" : "SLOT not found or not read"];
                            ++slotOdd[std::string(slotsId == 0 ? "no id" : "not found") + ", saved " + std::to_string(s.values.size())];
                        } else {
                            const auto containers = std::count(types->begin(), types->end(), 0);
                            const std::size_t saved = s.values.size();
                            if (saved != static_cast<std::size_t>(containers)) {
                                const bool person = d->definition.field(ts2::objd::field::kType) == ts2::sim::kPersonType;
                                ++slotOdd[std::string(person ? "person" : "object") + " saved " + std::to_string(saved) +
                                          " type-0 " + std::to_string(containers) + " all " + std::to_string(types->size())];
                            }
                            ++slotCounts[saved == static_cast<std::size_t>(containers) ? "equals type-0 entries"
                                         : saved == types->size()                     ? "equals all entries"
                                         : saved < static_cast<std::size_t>(containers) ? "fewer than type-0 entries"
                                                                                       : "more than type-0 entries"];
                        }
                    }
                    ++slotMarkers[s.marker < 0 ? (s.marker == -100 ? "-100" : "negative") : s.marker == 0 ? "0" : "positive"];
                    if (s.marker < 0) {
                        for (std::size_t i = 0; i < s.values.size(); ++i) {
                            const std::int16_t v = s.values[i];
                            if (v == 0) {
                                ++slotChecks["empty"];
                                continue;
                            }
                            const auto held = std::find_if(lot.objects.begin(), lot.objects.end(),
                                                           [v](const auto& x) { return x.id == static_cast<std::uint32_t>(v); });
                            if (held == lot.objects.end()) {
                                ++slotChecks["not an object on the lot"];
                            } else if (held->state.data[2] == static_cast<std::int16_t>(o.id) &&
                                       held->state.data[3] == static_cast<std::int16_t>(i)) {
                                ++slotChecks["object whose container id and slot number point back"];
                                // Tiles as the engine keeps them: whole x, y and the level.
                                const auto tile = [](const ts2::xobj::Header& h) {
                                    return std::array<int, 3>{static_cast<int>(h.location[0]),
                                                              static_cast<int>(h.location[1]), h.value1};
                                };
                                ++slotChecks[tile(held->state.header) == tile(o.state.header)
                                                 ? "held object on its container's tile"
                                                 : "held object on another tile"];
                            } else if (held->state.data[2] == static_cast<std::int16_t>(o.id)) {
                                ++slotChecks["object whose container id points back, other slot number"];
                            } else {
                                ++slotChecks["object that names another container"];
                            }
                        }
                    }
                }
                if (o.state.tail && o.type) {
                    const std::size_t n = o.state.tail->arrays.size();
                    ++arrayCounts[n];
                    ++iteratorsMatch[o.state.tail->shorts.size() == n];
                    arrayCountsByType[o.type->guid].insert(n);
                    if (const auto* d = definitionOf(o, code)) {
                        if (n > 0) {
                            arrayExamples.emplace(d->group, n);
                        }
                        ++field59[d->definition.field(59) == n ? "equal" : d->definition.field(59) > n ? "more" : "fewer"];
                        if (n > 0) {
                            ++withArraysDefined;
                            for (std::size_t i = 0; i < d->definition.fields.size(); ++i) {
                                fieldHits[i] += d->definition.fields[i] == n ? 1 : 0;
                            }
                        }
                    }
                }
                decodedTails += o.state.tail.has_value();
                decodedSims += o.state.person.has_value();
                frames += o.state.tail ? o.state.tail->stack.size() : 0;
                const auto* definition = definitionOf(o, code);
                withDefinition += definition != nullptr;
                persons += definition &&
                           definition->definition.field(ts2::objd::field::kType) == ts2::sim::kPersonType;
            }
            for (const auto& p : lot.problems) {
                if (++problems[p.message] <= 3) {
                    std::printf("  %s (version 0x%X) object %u: %s\n",
                                entry.path().filename().string().c_str(), lot.objectMap.version, p.id,
                                p.message.c_str());
                }
            }
        } catch (const std::exception& ex) {
            ++failed;
            std::printf("  %s: %s\n", entry.path().filename().string().c_str(), ex.what());
        }
    }
    std::printf("lots %zu (failed %zu), objects %zu, stale records %zu", lots, failed, objects, stale);
    if (code) {
        std::printf(", objects with a definition in the install %zu (persons %zu)", withDefinition,
                    persons);
    }
    std::printf("\nfully decoded %zu (Sims with motives and person data %zu), saved stack frames %zu\n",
                decodedTails, decodedSims, frames);
    for (const auto& [v, n] : versions) {
        std::printf("  version 0x%X: %zu lots\n", v, n);
    }
    std::size_t typesWithSeveralCounts = 0;
    for (const auto& [guid, counts] : arrayCountsByType) {
        typesWithSeveralCounts += counts.size() > 1 ? 1 : 0;
    }
    std::printf("  object arrays per object:");
    for (const auto& [n, count] : arrayCounts) {
        std::printf(" %zu=%zu", n, count);
    }
    std::printf("; object types with more than one count: %zu of %zu\n", typesWithSeveralCounts,
                arrayCountsByType.size());
    std::vector<std::pair<std::size_t, std::size_t>> best;
    for (const auto& [i, hits] : fieldHits) {
        best.emplace_back(hits, i);
    }
    std::sort(best.rbegin(), best.rend());
    std::printf("  shorts after the arrays, one per array: %zu, not: %zu\n", iteratorsMatch[1], iteratorsMatch[0]);
    std::printf("  slot blocks:");
    for (const auto& [k, n] : slotMarkers) {
        std::printf(" marker %s %zu", k.c_str(), n);
    }
    std::printf("\n");
    {
        std::vector<std::pair<std::size_t, std::string>> odd;
        for (const auto& [k, n] : slotOdd) {
            odd.emplace_back(n, k);
        }
        std::sort(odd.rbegin(), odd.rend());
        for (std::size_t i = 0; i < std::min<std::size_t>(odd.size(), 12); ++i) {
            std::printf("    %6zu  %s\n", odd[i].first, odd[i].second.c_str());
        }
    }
    for (const auto& [k, n] : slotCounts) {
        std::printf("  saved slot count: %s: %zu\n", k.c_str(), n);
    }
    for (const auto& [k, n] : multiTile) {
        std::printf("  multi-tile: %s: %zu\n", k.c_str(), n);
    }
    for (const auto& [k, n] : slotChecks) {
        std::printf("  slot values: %s: %zu\n", k.c_str(), n);
    }
    std::printf("  OBJD field 59 against the saved array count (all objects):");
    for (const auto& [k, n] : field59) {
        std::printf(" %s %zu", k.c_str(), n);
    }
    std::printf("\n");
    std::printf("  OBJD fields equal to the count, of %zu objects with arrays:", withArraysDefined);
    for (std::size_t k = 0; k < std::min<std::size_t>(best.size(), 5); ++k) {
        std::printf(" field %zu: %zu", best[k].second, best[k].first);
    }
    std::printf("\n");
    std::size_t shown = 0;
    for (const auto& [group, n] : arrayExamples) {
        if (shown++ < 12) {
            std::printf("    group 0x%08X: %zu arrays\n", group, n);
        }
    }
    for (const auto& [k, n] : frameChecks) {
        std::printf("  stack frame: %s: %zu\n", k.c_str(), n);
    }
    for (const auto& [k, n] : roomChecks) {
        std::printf("  placed objects, %s: %zu\n", k.c_str(), n);
    }
    for (const auto& [k, n] : problems) {
        std::printf("  problem: %s: %zu\n", k.c_str(), n);
    }
    return failed == 0 ? 0 : 1;
}

} // namespace

// For every saved stack frame under a folder of neighborhoods: how the frame's two object ids
// relate to the object whose stack it is and to the frame's stack object.
int showFrameSurvey(const std::filesystem::path& folder) {
    std::map<std::string, std::size_t> counts;
    std::size_t examples = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(folder)) {
        if (entry.path().extension() != ".package" || entry.path().parent_path().filename() != "Lots") {
            continue;
        }
        Lot lot;
        try {
            lot = ts2::content::loadLot(entry.path());
        } catch (const std::exception&) {
            continue;
        }
        for (const auto& o : lot.objects) {
            if (!o.state.tail) {
                continue;
            }
            for (const auto& f : o.state.tail->stack) {
                const auto self = static_cast<std::int16_t>(o.id);
                std::string key = std::string(f.value1 == self ? "first=self" : "first=other") +
                                  (f.value2 == self ? " second=self" : f.value2 == f.objectId ? " second=stack" : " second=other") +
                                  (o.state.person ? " (Sim)" : " (object)");
                ++counts[key];
                if (f.value1 != f.value2 && examples < 12) {
                    ++examples;
                    std::printf("  %s object %u (%s): first %d second %d stack %d tree 0x%04X owner %d\n",
                                entry.path().filename().string().c_str(), o.id, o.type ? o.type->name.c_str() : "-",
                                f.value1, f.value2, f.objectId, static_cast<std::uint16_t>(f.treeId), f.codeOwner);
                }
            }
        }
    }
    for (const auto& [key, n] : counts) {
        std::printf("%8zu  %s\n", n, key.c_str());
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "Usage:\n"
                     "  lotdump <lot.package> [--install <folder>]\n"
                     "  lotdump <lot.package> --object <id>\n"
                     "  lotdump <lot.package> --graphs\n"
                     "  lotdump <neighborhood-folder> --neighbors | --neighbor <id> [--install <folder>]\n"
                     "  lotdump <neighborhood-folder> --families\n"
                     "  lotdump <neighborhood-folder> --wants [id]\n"
                     "  lotdump <neighborhoods-folder> --survey [--install <folder>]\n"
                     "  lotdump <neighborhoods-folder> --frame-survey\n");
        return 1;
    }
    try {
        bool surveyMode = false;
        bool neighborsMode = false;
        std::optional<std::int16_t> neighbor;
        std::optional<std::filesystem::path> install;
        for (int i = 2; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--survey") {
                surveyMode = true;
            } else if (arg == "--wants") {
                std::optional<std::int16_t> id;
                if (i + 1 < argc) {
                    id = static_cast<std::int16_t>(std::stoi(argv[++i], nullptr, 0));
                }
                return showWants(std::filesystem::path(argv[1]), id);
            } else if (arg == "--families") {
                return showFamilies(std::filesystem::path(argv[1]));
            } else if (arg == "--neighbors") {
                neighborsMode = true;
            } else if (arg == "--neighbor" && i + 1 < argc) {
                neighborsMode = true;
                neighbor = static_cast<std::int16_t>(std::stoi(argv[++i], nullptr, 0));
            } else if (arg == "--install" && i + 1 < argc) {
                install = std::filesystem::path(argv[++i]);
            } else if (arg == "--object" && i + 1 < argc) {
                return showObject(std::filesystem::path(argv[1]), static_cast<std::uint32_t>(std::stoul(argv[++i])));
            } else if (arg == "--frame-survey") {
                return showFrameSurvey(std::filesystem::path(argv[1]));
            } else if (arg == "--graphs") {
                return showGraphs(std::filesystem::path(argv[1]));
            } else {
                std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
                return 1;
            }
        }

        ts2::content::ResourceIndex index;
        std::unique_ptr<IndexCodeSource> code;
        if (install) {
            for (const auto& path : ts2::content::objectLoadOrder(ts2::content::findProducts(*install))) {
                index.addPackage(path);
            }
            code = std::make_unique<IndexCodeSource>(index);
        }
        const std::filesystem::path target(argv[1]);
        if (neighborsMode) {
            ts2::content::addCharacters(index, target);
            if (!code) {
                code = std::make_unique<IndexCodeSource>(index);
            }
            return showNeighbors(target, *code, neighbor);
        }
        return surveyMode ? survey(target, code.get(), install ? &index : nullptr)
                          : listLot(target, code.get());
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
