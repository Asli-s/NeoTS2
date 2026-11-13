#include "formats/dbpf.hpp"
#include "formats/dbpf_writer.hpp"
#include "formats/qfs.hpp"
#include "formats/simi.hpp"
#include "formats/objm.hpp"
#include "formats/xobj.hpp"
#include "formats/cobject_record.hpp"
#include "formats/srel.hpp"
#include "formats/sdsc.hpp"
#include "formats/fami.hpp"
#include "formats/famt.hpp"
#include "formats/ngbh.hpp"
#include "formats/lot_grid.hpp"
#include "formats/wallgraph.hpp"
#include "formats/walllayer.hpp"
#include "formats/fencepostlayer.hpp"
#include "formats/stringmap.hpp"
#include "formats/roofs.hpp"
#include "formats/terrain.hpp"
#include "formats/objt.hpp"
#include "formats/world_db.hpp"
#include "formats/lot_records.hpp"
#include "formats/nhood_objects.hpp"
#include "formats/nhood_terrain.hpp"
#include "formats/saved_camera.hpp"
#include "formats/lot_description.hpp"
#include "formats/resource_types.hpp"
#include "formats/str.hpp"
#include "formats/object_archive.hpp"
#include "formats/luascript.hpp"
#include "formats/swaf.hpp"
#include "formats/propset.hpp"
#include "formats/datatables.hpp"
#include "formats/ttab.hpp"

#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using ts2::types::kStringTable;

struct TypeStats {
    std::size_t count = 0;
    std::size_t compressed = 0;
    std::uint64_t bytesOnDisk = 0;
};

void printHeader(const ts2::dbpf::Header& h) {
    std::printf("DBPF version  : %u.%u\n", h.majorVersion, h.minorVersion);
    std::printf("Index version : %u.%u\n", h.indexMajorVersion, h.indexMinorVersion);
    std::printf("Resources     : %u\n", h.indexEntryCount);
    std::printf("Holes         : %u\n\n", h.holeCount);
}

void printSummary(const ts2::dbpf::Package& package) {
    std::map<std::uint32_t, TypeStats> stats;
    for (const auto& e : package.entries()) {
        TypeStats& s = stats[e.key.type];
        ++s.count;
        s.compressed += e.compressed ? 1 : 0;
        s.bytesOnDisk += e.size;
    }

    std::printf("%-12s %8s %11s %12s\n", "Type", "Count", "Compressed", "KB on disk");
    for (const auto& [type, s] : stats) {
        std::printf("%-12s %8zu %11zu %12llu\n",
                    ts2::types::typeName(type).c_str(), s.count, s.compressed,
                    static_cast<unsigned long long>(s.bytesOnDisk / 1024));
    }
}

void printList(const ts2::dbpf::Package& package) {
    std::printf("%-12s %-10s %-10s %-10s %10s %10s\n",
                "Type", "Group", "Instance", "InstHigh", "Size", "Unpacked");
    for (const auto& e : package.entries()) {
        std::printf("%-12s 0x%08X 0x%08X 0x%08X %10u %10u%s\n",
                    ts2::types::typeName(e.key.type).c_str(), e.key.group,
                    e.key.instance, e.key.instanceHigh, e.size,
                    e.uncompressedSize, e.compressed ? "  (compressed)" : "");
    }
}

int verifyDecompression(ts2::dbpf::Package& package) {
    std::size_t ok = 0;
    std::size_t failed = 0;
    std::uint64_t packedBytes = 0;
    std::uint64_t unpackedBytes = 0;

    const auto start = std::chrono::steady_clock::now();
    for (const auto& e : package.entries()) {
        if (!e.compressed) {
            continue;
        }
        try {
            const std::vector<std::uint8_t> data = package.read(e);
            if (data.size() != e.uncompressedSize) {
                throw std::runtime_error("size " + std::to_string(data.size()) +
                                         " != expected " + std::to_string(e.uncompressedSize));
            }
            ++ok;
            packedBytes += e.size;
            unpackedBytes += data.size();
        } catch (const std::exception& ex) {
            if (failed < 10) {
                std::printf("FAIL %s 0x%08X 0x%08X: %s\n",
                            ts2::types::typeName(e.key.type).c_str(),
                            e.key.group, e.key.instance, ex.what());
            }
            ++failed;
        }
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start);

    std::printf("Decompressed  : %zu ok, %zu failed\n", ok, failed);
    std::printf("Size          : %llu KB -> %llu KB\n",
                static_cast<unsigned long long>(packedBytes / 1024),
                static_cast<unsigned long long>(unpackedBytes / 1024));
    std::printf("Time          : %.0f ms\n", elapsed.count());
    return failed == 0 ? 0 : 1;
}

int grep(ts2::dbpf::Package& package, std::string_view text) {
    std::size_t hits = 0;
    for (const auto& e : package.entries()) {
        std::vector<std::uint8_t> data;
        try {
            data = package.read(e);
        } catch (const std::exception&) {
            continue;
        }
        const auto it = std::search(data.begin(), data.end(), text.begin(), text.end());
        if (it == data.end()) {
            continue;
        }
        const auto offset = static_cast<std::size_t>(it - data.begin());
        const std::size_t limit = std::min<std::size_t>(data.size(), 64);
        const auto nameEnd = std::find(data.begin(), data.begin() + limit, std::uint8_t{0});
        std::printf("%-8s 0x%08X 0x%08X  at %-6zu \"%s\"\n",
                    ts2::types::typeName(e.key.type).c_str(), e.key.group, e.key.instance,
                    offset, std::string(data.begin(), nameEnd).c_str());
        ++hits;
    }
    std::printf("%zu resources contain \"%.*s\"\n", hits, static_cast<int>(text.size()), text.data());
    return 0;
}

int showStrings(ts2::dbpf::Package& package, std::uint32_t group, std::uint32_t instance) {
    for (const auto& e : package.entries()) {
        if (e.key.type != kStringTable || e.key.group != group || e.key.instance != instance) {
            continue;
        }
        const ts2::str::StringTable table = ts2::str::parse(package.read(e));
        const auto english = table.forLanguage(1);
        std::printf("\"%s\"  format 0x%04X, %zu entries (%zu US English)\n\n",
                    table.name.c_str(), table.format, table.entries.size(), english.size());
        for (std::size_t i = 0; i < english.size(); ++i) {
            std::printf("0x%02zX  %-40s %s\n", i, english[i]->value.c_str(),
                        english[i]->description.c_str());
        }
        return 0;
    }
    std::fprintf(stderr, "no STR# with group 0x%08X instance 0x%04X\n", group, instance);
    return 1;
}

int hexDump(ts2::dbpf::Package& package, std::uint32_t type, std::uint32_t group,
            std::uint32_t instance, std::size_t maxBytes) {
    for (const auto& e : package.entries()) {
        if (e.key.type != type || e.key.group != group || e.key.instance != instance) {
            continue;
        }
        const std::vector<std::uint8_t> data = package.read(e);
        std::printf("%zu bytes\n", data.size());
        for (std::size_t row = 0; row < std::min(data.size(), maxBytes); row += 16) {
            std::printf("%06zX  ", row);
            for (std::size_t i = row; i < row + 16; ++i) {
                if (i < data.size()) {
                    std::printf("%02X ", data[i]);
                } else {
                    std::printf("   ");
                }
            }
            std::printf(" ");
            for (std::size_t i = row; i < std::min(row + 16, data.size()); ++i) {
                std::printf("%c", data[i] >= 0x20 && data[i] < 0x7F ? data[i] : '.');
            }
            std::printf("\n");
        }
        return 0;
    }
    std::fprintf(stderr, "resource not found\n");
    return 1;
}

int extract(ts2::dbpf::Package& package, std::uint32_t type, const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    std::size_t written = 0;
    for (const auto& e : package.entries()) {
        if (e.key.type != type) {
            continue;
        }
        const std::vector<std::uint8_t> data = package.read(e);
        char name[64];
        std::snprintf(name, sizeof(name), "%08X_%08X_%08X.bin", e.key.group, e.key.instanceHigh,
                      e.key.instance);
        std::ofstream out(folder / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        ++written;
    }
    std::printf("%zu resources written\n", written);
    return 0;
}

int surveyStrings(ts2::dbpf::Package& package) {
    std::map<std::uint16_t, std::size_t> formats;
    std::size_t ok = 0;
    std::size_t failed = 0;
    std::size_t unsupported = 0;
    std::size_t strings = 0;
    for (const auto& e : package.entries()) {
        if (e.key.type != kStringTable) {
            continue;
        }
        try {
            const ts2::str::StringTable table = ts2::str::parse(package.read(e));
            ++formats[table.format];
            if (!table.supported) {
                ++unsupported;
                continue;
            }
            strings += table.entries.size();
            ++ok;
        } catch (const std::exception& ex) {
            if (failed < 10) {
                std::printf("FAIL 0x%08X 0x%08X: %s\n", e.key.group, e.key.instance, ex.what());
            }
            ++failed;
        }
    }
    std::printf("STR# parsed: %zu ok, %zu unsupported format, %zu failed, %zu strings\n",
                ok, unsupported, failed, strings);
    for (const auto& [format, n] : formats) {
        std::printf("  format 0x%04X: %zu\n", format, n);
    }
    return failed == 0 ? 0 : 1;
}

// Loads every compiled Lua script of the package into Lua 5.0 (without running it).
int surveyLuaScripts(ts2::dbpf::Package& package) {
    std::size_t modules = 0, scripts = 0, failed = 0;
    for (const auto& e : package.entries()) {
        if (e.key.type != ts2::types::kLuaModule && e.key.type != ts2::types::kLuaScript) {
            continue;
        }
        (e.key.type == ts2::types::kLuaModule ? modules : scripts) += 1;
        try {
            const auto script = ts2::luascript::parse(package.read(e));
            const auto chunk = ts2::luascript::nativeChunk(script.chunk);
            lua_State* L = lua_open();
            const int status = luaL_loadbuffer(L, reinterpret_cast<const char*>(chunk.data()), chunk.size(),
                                               script.name.c_str());
            if (status != 0) {
                ++failed;
                std::printf("  %s: %s\n", script.name.c_str(), lua_tostring(L, -1));
            }
            lua_close(L);
        } catch (const std::exception& ex) {
            ++failed;
            std::printf("  0x%08X: %s\n", e.key.instance, ex.what());
        }
    }
    std::printf("%zu modules, %zu scripts, %zu failed to load\n", modules, scripts, failed);
    return failed == 0 ? 0 : 1;
}

// Reads every interaction table (TTAB) of the package and counts versions and leftovers.
int surveyInteractionTables(ts2::dbpf::Package& package) {
    std::map<std::uint32_t, std::size_t> versions;
    std::size_t tables = 0, withExtra = 0, failed = 0, interactions = 0;
    for (const auto& e : package.entries()) {
        if (e.key.type != ts2::types::kInteractionTable) {
            continue;
        }
        ++tables;
        try {
            const auto t = ts2::ttab::parse(package.read(e));
            ++versions[t.version];
            interactions += t.interactions.size();
            withExtra += t.extraBytes != 0 ? 1 : 0;
        } catch (const std::exception& ex) {
            ++failed;
            std::printf("  0x%08X 0x%08X: %s\n", e.key.group, e.key.instance, ex.what());
        }
    }
    std::printf("%zu tables, %zu interactions, %zu failed, %zu with bytes left over\n", tables, interactions,
                failed, withExtra);
    for (const auto& [v, n] : versions) {
        std::printf("  version 0x%X: %zu\n", v, n);
    }
    return failed == 0 ? 0 : 1;
}

// Reads every script data table record of the package and counts tables, values and leftovers.
int surveyDataTables(ts2::dbpf::Package& package) {
    std::size_t records = 0, tables = 0, values = 0, leftovers = 0, failed = 0;
    std::map<std::uint32_t, std::size_t> resourceIds;
    for (const auto& e : package.entries()) {
        if (e.key.type != ts2::types::kDataTables) {
            continue;
        }
        ++records;
        ++resourceIds[e.key.instanceHigh];
        try {
            const auto d = ts2::datatables::parse(package.read(e));
            tables += d.tables.size();
            for (const auto& t : d.tables) {
                values += t.values.size();
            }
            leftovers += d.extraBytes != 0 ? 1 : 0;
        } catch (const std::exception& ex) {
            ++failed;
            std::printf("  0x%08X: %s\n", e.key.instance, ex.what());
        }
    }
    std::printf("%zu records, %zu tables, %zu values, %zu with leftover bytes, %zu failed;", records, tables, values,
                leftovers, failed);
    for (const auto& [id, n] : resourceIds) {
        std::printf(" resource 0x%08X: %zu", id, n);
    }
    std::printf("\n");
    return failed == 0 ? 0 : 1;
}

void printDataValue(const ts2::datatables::Value& v) {
    using Type = ts2::datatables::Value::Type;
    switch (v.type) {
    case Type::Float: std::printf("%g", v.f); break;
    case Type::Int: std::printf("%d", v.i); break;
    case Type::Unsigned: std::printf("%u", v.u); break;
    case Type::Bool: std::printf(v.b ? "true" : "false"); break;
    case Type::String: std::printf("\"%s\"", v.s.c_str()); break;
    case Type::Table:
        std::printf("{");
        for (const auto& e : v.table) {
            printDataValue(e.key);
            std::printf("=");
            printDataValue(e.value);
            std::printf(" ");
        }
        std::printf("}");
        break;
    }
}

// Prints every script data table whose name contains `name`, with the object it belongs to.
int printDataTables(ts2::dbpf::Package& package, std::string_view name) {
    for (const auto& e : package.entries()) {
        if (e.key.type != ts2::types::kDataTables) {
            continue;
        }
        for (const auto& t : ts2::datatables::parse(package.read(e)).tables) {
            if (t.name.find(name) == std::string::npos) {
                continue;
            }
            std::printf("object %u table %u \"%s\":", e.key.instance, t.id, t.name.c_str());
            for (const auto& v : t.values) {
                std::printf(" ");
                printDataValue(v.key);
                std::printf("=");
                printDataValue(v.value);
            }
            std::printf("\n");
        }
    }
    return 0;
}

// Reads the neighborhood record (NGBH) and summarizes its inventories.
int showNeighborhoodRecord(ts2::dbpf::Package& package) {
    for (const auto& e : package.entries()) {
        if (e.key.type != ts2::types::kNeighborhoodRecord) {
            continue;
        }
        const auto hood = ts2::ngbh::parse(package.read(e));
        auto tokens = [](const ts2::ngbh::Inventory& inv) { return inv.counted.size() + inv.singular.size(); };
        std::size_t lotTokens = 0, familyTokens = 0, neighborTokens = 0;
        for (const auto& [id, inv] : hood.lots) lotTokens += tokens(inv);
        for (const auto& [id, inv] : hood.families) familyTokens += tokens(inv);
        for (const auto& [id, inv] : hood.neighbors) neighborTokens += tokens(inv);
        std::printf("version 0x%X, terrain %s, %zu extra bytes\n", hood.version, hood.terrain.c_str(), hood.extraBytes);
        std::printf("global: %zu counted, %zu singular\n", hood.global.counted.size(), hood.global.singular.size());
        std::printf("lots: %zu (%zu tokens), families: %zu (%zu tokens), neighbors: %zu (%zu tokens)\n",
                    hood.lots.size(), lotTokens, hood.families.size(), familyTokens, hood.neighbors.size(),
                    neighborTokens);
        return hood.extraBytes == 0 ? 0 : 1;
    }
    std::fprintf(stderr, "no NGBH resource\n");
    return 1;
}

struct SaveCheck {
    int files = 0;
    int refused = 0;          // packages the save writer would not read
    int rewrittenSame = 0;    // packed packages that write back to the same bytes
    int rewrittenOther = 0;   // packages with free space or spare bytes, which write back smaller
    std::size_t resources = 0;
    std::size_t packedSame = 0;      // compressed resources the compressor gives back exactly
    std::size_t packedOther = 0;
    std::size_t ruleSame = 0;        // resources stored as the game's rule stores them
    std::size_t ruleOther = 0;
    // Records a writer exists for: written back to the same bytes, differently, or of a version it
    // doesn't write.
    std::map<std::string, std::array<std::size_t, 4>> records;  // the fourth: as shipped, never written by the game
    // With a copy of the shipped folders: resources stored as they shipped were never written by the
    // game, so they are set apart.
    std::filesystem::path root;
    std::filesystem::path pristine;
    std::size_t shipped = 0;
};

// Writes a record back with its writer when one exists. Returns the name of the record kind, or
// empty when none applies, and sets `same` when the bytes match or `skipped` for an unhandled version.
// Wants records that read but keep bytes after their history, counted apart.
std::size_t leftOverRecords = 0;
// Sims read at their lot's version whose saved interactions did not read, and all Sims read.
std::size_t unreadSimStates = 0;
std::size_t readSims = 0;

std::string checkRecord(std::uint32_t type, std::span<const std::uint8_t> body, std::uint32_t lotVersion, bool& same, bool& skipped) {
    // Saved objects take their layout from the lot's version in its object map.
    const std::pair<std::uint32_t, ts2::xobj::RecordKind> objects[] = {{0x584F424A, ts2::xobj::RecordKind::Object},
                                                                       {0x584D544F, ts2::xobj::RecordKind::ObjectWithMaterial},
                                                                       {0x50455253, ts2::xobj::RecordKind::Person}};
    for (const auto& [objectType, kind] : objects) {
        if (type != objectType) {
            continue;
        }
        skipped = lotVersion != ts2::xobj::kWrittenVersion;
        if (kind == ts2::xobj::RecordKind::Person) {
            try {
                const auto person = ts2::xobj::parse(body, lotVersion, kind);
                ++readSims;
                if (person.person && person.person->stateUnread) {
                    ++unreadSimStates;
                    std::printf("  a Sim's saved interactions did not read (lot version 0x%X)\n", lotVersion);
                }
            } catch (const std::exception&) {
            }
        }
        if (!skipped) {
            try {
                const auto object = ts2::xobj::parse(body, lotVersion, kind);
                if (!object.rest.empty()) {
                    std::printf("  %zu bytes left after a saved object's parts\n", object.rest.size());
                }
                const auto written = ts2::xobj::write(object, kind);
                same = written == std::vector<std::uint8_t>(body.begin(), body.end());
                if (!same) {
                    std::size_t at = 0;
                    while (at < written.size() && at < body.size() && written[at] == body[at]) {
                        ++at;
                    }
                    std::printf("  record of %zu bytes written as %zu, first difference at 0x%zX\n", body.size(), written.size(), at);
                }
            } catch (const std::exception&) {
                // A record the game writes with the lot's version reads with it. One that doesn't was
                // left by an older save: records of deleted objects stay, and their ids are reused.
                skipped = true;
            }
        }
        return kind == ts2::xobj::RecordKind::Person ? "PERS" : kind == ts2::xobj::RecordKind::Object ? "XOBJ" : "XMTO";
    }
    if (type == ts2::simi::kType) {
        const auto record = ts2::simi::readRecord(body);
        skipped = !record && !(body.size() >= 0x48 && body[0x44] == ts2::simi::kWrittenVersion && body[0x45] == 0);
        same = record && ts2::simi::writeRecord(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "SIMI";
    }
    if (type == ts2::nhood::kLotInfoType) {
        const auto record = ts2::nhood::readLotInfo(body);
        skipped = !record && !(body.size() >= 66 && body[64] == ts2::nhood::kLotInfoWrittenVersion && body[65] == 0);
        same = record && ts2::nhood::writeLotInfo(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "lot info";
    }
    // The wall records hold the version at 68.
    const std::uint32_t recordVersion = body.size() >= 72 ? static_cast<std::uint32_t>(body[68] | body[69] << 8 | body[70] << 16 | body[71] << 24) : 0;
    const std::vector<std::uint8_t> whole(body.begin(), body.end());
    try {
        if (type == ts2::wallgraph::kType) {
            skipped = recordVersion != ts2::wallgraph::kWrittenVersion;
            same = !skipped && ts2::wallgraph::write(ts2::wallgraph::parse(body)) == whole;
            return "wall graph";
        }
        if (type == ts2::stringmap::kType) {
            skipped = recordVersion != 1;
            same = !skipped && ts2::stringmap::write(ts2::stringmap::parse(body)) == whole;
            return "string map";
        }
        if (type == ts2::roofs::kType) {
            skipped = recordVersion != ts2::roofs::kWrittenVersion;
            same = !skipped && ts2::roofs::write(ts2::roofs::parse(body)) == whole;
            return "roofs";
        }
        if (type == ts2::types::kLotTexture) {
            skipped = recordVersion != ts2::terrain::kLotTextureWrittenVersion;
            same = !skipped && ts2::terrain::writeLotTexture(ts2::terrain::parseLotTexture(body)) == whole;
            return "lot texture";
        }
        if (type == 0x6F626A74) {
            skipped = recordVersion != ts2::objt::kWrittenVersion;
            same = !skipped && ts2::objt::write(ts2::objt::parse(body)) == whole;
            return "object types";
        }
        if (type == ts2::worlddb::kType) {
            const auto record = ts2::worlddb::readRecord(body);
            skipped = !record && recordVersion != ts2::worlddb::kWrittenVersion;
            same = record && ts2::worlddb::writeRecord(*record) == whole;
            return "world database";
        }
        if (type == ts2::walllayer::kType) {
            same = ts2::walllayer::write(ts2::walllayer::parse(body)) == whole;
            return "wall layer";
        }
        if (type == ts2::fencepostlayer::kType) {
            same = ts2::fencepostlayer::write(ts2::fencepostlayer::parse(body)) == whole;
            return "fence post layer";
        }
    } catch (const std::exception&) {
        same = false;
        return type == ts2::wallgraph::kType      ? "wall graph"
               : type == ts2::walllayer::kType    ? "wall layer"
               : type == ts2::stringmap::kType    ? "string map"
               : type == ts2::roofs::kType        ? "roofs"
               : type == ts2::types::kLotTexture  ? "lot texture"
               : type == 0x6F626A74               ? "object types"
                                                  : "fence post layer";
    }
    const auto writesBack = [&body](const std::optional<std::vector<std::uint8_t>>& written) {
        return written && *written == std::vector<std::uint8_t>(body.begin(), body.end());
    };
    if (type == ts2::lotrecords::kHouseType) {
        const auto r = ts2::lotrecords::readHouse(body);
        // Older games wrote versions 0x4E and 0x4F.
        skipped = !r && !(body.size() >= 72 && body[68] == 0x55 && body[69] == 0 && body[70] == 0 && body[71] == 0);
        same = writesBack(r ? std::optional(ts2::lotrecords::writeHouse(*r)) : std::nullopt);
        return "house";
    }
    if (type == ts2::lotrecords::kPairTableType) {
        // Each instance holds one kind, and only the right kind reads.
        for (const bool texts : {false, true}) {
            if (const auto r = ts2::lotrecords::readPairTable(body, texts); r && writesBack(ts2::lotrecords::writePairTable(*r))) {
                same = true;
            }
        }
        return "pair table";
    }
    if (type == ts2::lotrecords::kNineWordTableType || type == ts2::lotrecords::kFourWordTableType) {
        const auto r = ts2::lotrecords::readWordTable(body);
        same = writesBack(r ? std::optional(ts2::lotrecords::writeWordTable(*r)) : std::nullopt);
        return "word table";
    }
    if (type == ts2::lotrecords::kVertexLayerType) {
        const auto r = ts2::lotrecords::readVertexLayer(body);
        same = writesBack(r ? std::optional(ts2::lotrecords::writeVertexLayer(*r)) : std::nullopt);
        return "vertex layer";
    }
    if (type == ts2::lotrecords::kRoutingAggregateType) {
        const auto r = ts2::lotrecords::readRoutingAggregate(body);
        // Older games wrote versions 2, 4 and 5.
        skipped = !r && recordVersion != 6;
        same = writesBack(r ? std::optional(ts2::lotrecords::writeRoutingAggregate(*r)) : std::nullopt);
        return "routing aggregate";
    }
    if (type == ts2::lotrecords::kSgSystemType) {
        const auto r = ts2::lotrecords::readSgSystem(body);
        same = writesBack(r ? std::optional(ts2::lotrecords::writeSgSystem(*r)) : std::nullopt);
        return "SG system";
    }
    if (type == ts2::fami::kType) {
        // Families of older versions were written by older games.
        skipped = !(body.size() >= 8 && body[4] == ts2::fami::kWrittenVersion && body[5] == 0 && body[6] == 0 && body[7] == 0);
        try {
            same = !skipped && ts2::fami::write(ts2::fami::parse(body)) == std::vector<std::uint8_t>(body.begin(), body.end());
        } catch (const std::exception&) {
            same = false;
        }
        return "family";
    }
    if (type == 0x0C560F39 || type == 0xEBFEE342) {
        const auto record = ts2::propset::readBinary(body);
        same = record && ts2::propset::writeBinary(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "property set";
    }
    if (type == ts2::lotrecords::kDnaIndexType) {
        const auto record = ts2::lotrecords::readDnaIndex(body);
        same = record && ts2::lotrecords::writeDnaIndex(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "Sim DNA index";
    }
    if (type == ts2::nhood::kTerrainType) {
        const auto record = ts2::nhood::readTerrainRecord(body);
        // Older games wrote other versions.
        skipped = !record && !(body.size() >= 8 && body[4] == ts2::nhood::kTerrainWrittenVersion && body[5] == 0);
        same = record && ts2::nhood::writeTerrainRecord(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "neighborhood terrain";
    }
    if (type == ts2::nhood::kObjectsType) {
        const auto record = ts2::nhood::readObjectsRecord(body);
        // Older games wrote other versions.
        skipped = !record && !(body.size() >= 4 && body[0] == ts2::nhood::kObjectsWrittenVersion && body[1] == 0);
        same = record && ts2::nhood::writeObjectsRecord(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "neighborhood objects";
    }
    if (type == ts2::archive::kType) {
        const auto record = ts2::archive::readRecord(body);
        // Older games wrote other versions of the objects inside.
        skipped = !record && !(body.size() >= 8 && body[0] == ts2::archive::kWrittenVersion && body[4] == ts2::xobj::kWrittenVersion &&
                               body[1] == 0 && body[5] == 0);
        same = record && ts2::archive::writeRecord(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "object archive";
    }
    if (type == 0x53545223 || type == 0x43545353 || type == 0x54544173) {
        const auto record = ts2::str::readRecord(body);
        // Other tools wrote other formats.
        skipped = !record && !(body.size() >= 66 && body[64] == 0xFD && body[65] == 0xFF);
        same = record && ts2::str::writeRecord(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "string table";
    }
    // Sim DNA, and outfit and age records written in the text form.
    if ((type == 0xEBFEE33F || type == 0xEBCF3E27 || type == 0xAC598EAC) && body.size() >= 5 &&
        std::string_view(reinterpret_cast<const char*>(body.data()), 5) == "<?xml") {
        const auto record = ts2::propset::readXml(body);
        same = record && ts2::propset::writeXml(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return type == 0xEBFEE33F ? "Sim DNA" : "text property set";
    }
    if (type == ts2::swaf::kType) {
        const auto record = ts2::swaf::readRecord(body);
        // Older games wrote other versions of the record and of its wants. Records with bytes after
        // the history are not written yet.
        std::size_t leftOver = 0;
        const bool withLeftOver = !record && ts2::swaf::readRecord(body, &leftOver) && leftOver != 0;
        if (withLeftOver) {
            ++leftOverRecords;
        }
        skipped = !record && (withLeftOver || !(body.size() >= 4 && body[0] == ts2::swaf::kWrittenVersion && body[1] == 0 && body[2] == 0 && body[3] == 0));
        same = record && ts2::swaf::writeRecord(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "wants";
    }
    if (type == ts2::ngbh::kType) {
        const auto record = ts2::ngbh::readRecord(body);
        skipped = !record && !(body.size() >= 8 && body[4] == ts2::ngbh::kWrittenVersion && body[5] == 0 && body[6] == 0 && body[7] == 0);
        same = record && ts2::ngbh::writeRecord(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "neighborhood";
    }
    if (type == ts2::famt::kType) {
        skipped = !(body.size() >= 4 && body[0] == ts2::famt::kWrittenVersion && body[1] == 0 && body[2] == 0 && body[3] == 0);
        try {
            same = !skipped && ts2::famt::write(ts2::famt::parse(body)) == std::vector<std::uint8_t>(body.begin(), body.end());
        } catch (const std::exception&) {
            same = false;
        }
        return "family ties";
    }
    if (type == ts2::fami::kHistoryType) {
        const auto r = ts2::fami::readHistory(body);
        skipped = !r && !(body.size() >= 8 && body[4] == 0x55 && body[5] == 0 && body[6] == 0 && body[7] == 0);
        same = writesBack(r ? std::optional(ts2::fami::writeHistory(*r)) : std::nullopt);
        return "family history";
    }
    if (type == ts2::lotrecords::kNeighborhoodInfoType) {
        const auto r = ts2::lotrecords::readNeighborhoodInfo(body);
        skipped = !r && !(body.size() >= 4 && body[0] == 10 && body[1] == 0 && body[2] == 0 && body[3] == 0);
        same = writesBack(r ? std::optional(ts2::lotrecords::writeNeighborhoodInfo(*r)) : std::nullopt);
        return "neighborhood info";
    }
    if (type == ts2::lotrecords::kShortRecordType) {
        const auto r = ts2::lotrecords::readShortRecord(body);
        // Older games wrote other versions.
        skipped = !r && !(body.size() >= 8 && body[4] == 0xCB && body[5] == 0 && body[6] == 0 && body[7] == 0);
        same = writesBack(r ? std::optional(ts2::lotrecords::writeShortRecord(*r)) : std::nullopt);
        return "short record";
    }
    if (type == ts2::lotrecords::kKeyedListsType) {
        const auto r = ts2::lotrecords::readKeyedLists(body);
        same = writesBack(r ? std::optional(ts2::lotrecords::writeKeyedLists(*r)) : std::nullopt);
        return "keyed lists";
    }
    if (type == ts2::lotrecords::kArchiveCounterType) {
        const auto r = ts2::lotrecords::readArchiveCounter(body);
        same = writesBack(r ? std::optional(ts2::lotrecords::writeArchiveCounter(*r)) : std::nullopt);
        return "archive counter";
    }
    if (type == ts2::lotrecords::kFlagRecordType) {
        const auto r = ts2::lotrecords::readFlagRecord(body);
        same = writesBack(r ? std::optional(ts2::lotrecords::writeFlagRecord(*r)) : std::nullopt);
        return "flag record";
    }
    if (type == ts2::lotgrid::k2DType || type == ts2::lotgrid::k3DType) {
        const auto record = ts2::lotgrid::readGrid(body);
        same = record && ts2::lotgrid::writeGrid(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "grid";
    }
    if (type == ts2::sdsc::kType) {
        const auto record = ts2::sdsc::readRecord(body);
        // Older games wrote other versions, and version 0x71 with older person data.
        skipped = !record && !(body.size() >= 8 && body[0] == ts2::sdsc::kWrittenVersion && body[1] == 0 && body[2] == 0 && body[3] == 0 &&
                               body[4] == ts2::sdsc::kWrittenPersonDataVersion && body[5] == 0 && body[6] == 0 && body[7] == 0);
        same = record && ts2::sdsc::writeRecord(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "Sim description";
    }
    if (type == ts2::nhood::kLotDescriptionType) {
        const auto record = ts2::nhood::readLotDescriptionRecord(body);
        skipped = !record && !(body.size() >= 2 && body[0] == ts2::nhood::kLotDescriptionWrittenVersion && body[1] == 0);
        same = record && ts2::nhood::writeLotDescriptionRecord(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "lot description";
    }
    if (type == ts2::srel::kType) {
        const auto record = ts2::srel::readRecord(body);
        // The game writes nothing after the values. Records with bytes after them, or of version 3,
        // were written by older games.
        skipped = !record;
        same = record && ts2::srel::writeRecord(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "relationship";
    }
    if (type == ts2::cobject::kType) {
        const auto record = ts2::cobject::readRecord(body);
        // Records of objects that were not saved again keep the older class versions they were
        // written with, and so do records of deleted objects and of lots an older game last saved. A
        // record of a live object, in a lot this game saved and with today's class versions, that does
        // not read counts as different.
        bool current = true;
        std::size_t at = 64;
        for (int n = 0; n < 8; ++n) {
            if (at + 9 > body.size()) {
                current = false;
                break;
            }
            const auto word = [&](std::size_t p) { return static_cast<std::uint32_t>(body[p] | body[p + 1] << 8 | body[p + 2] << 16 | body[p + 3] << 24); };
            const std::uint32_t tag = word(at), version = word(at + 4);
            const bool known = (tag == 0xFA34F3EB && version == 3) || (tag == 0xFD4F2EC7 && version == 4) ||
                               (tag == 0x7D4F2BB8 && version == 0x10) || (tag == ts2::cobject::kType && version == 0x11);
            if (!known) {
                current = false;
                break;
            }
            if (tag == ts2::cobject::kType) {
                break;
            }
            at += 9 + body[at + 8];
        }
        skipped = !record && (!current || lotVersion != ts2::objm::kWrittenVersion);
        same = record && ts2::cobject::writeRecord(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "scene";
    }
    if (type == ts2::savedcamera::kType) {
        const auto record = ts2::savedcamera::readRecord(body);
        skipped = !record && !(body.size() >= 65 && (body[64] == ts2::savedcamera::kOrbitKind || body[64] == ts2::savedcamera::kFreeKind));
        same = record && ts2::savedcamera::writeRecord(*record) == std::vector<std::uint8_t>(body.begin(), body.end());
        return "camera";
    }
    if (type == 0x4F626A4D) {
        const std::vector<std::uint8_t> bytes(body.begin(), body.end());
        const std::uint32_t version = body.size() >= 72 ? static_cast<std::uint32_t>(body[68] | body[69] << 8) : 0;
        if (body.size() >= 68 && body[64] == ts2::objm::kGroupsWrittenVersion && body[65] == 0 && body[66] == 0 && body[67] == 0) {
            const auto record = ts2::objm::readGroupsRecord(body);
            same = record && ts2::objm::writeGroupsRecord(*record) == bytes;
            return "object groups";
        }
        const auto record = ts2::objm::readMapRecord(body);
        skipped = !record && version != ts2::objm::kWrittenVersion;
        same = record && ts2::objm::writeMapRecord(*record) == bytes;
        return "object map";
    }
    return {};
}

// Checks the save writer against packages the game wrote: every compressed resource compresses
// again to its stored bytes, every resource is stored as storeResource would store it, and the
// package written back holds the same resources (byte for byte when the game had packed it).
void checkSavedPackage(const std::filesystem::path& path, SaveCheck& total) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ++total.files;
    ts2::dbpf::PackageImage image;
    try {
        image = ts2::dbpf::readImage(file);
    } catch (const std::exception& e) {
        ++total.refused;
        std::printf("refused %s: %s\n", path.string().c_str(), e.what());
        return;
    }
    const std::vector<std::uint8_t> written = ts2::dbpf::writeImage(image);
    ts2::dbpf::verifyImage(written, image);
    if (written == file) {
        ++total.rewrittenSame;
    } else {
        ++total.rewrittenOther;
    }
    std::size_t packedOther = 0;
    std::size_t ruleOther = 0;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>, std::vector<std::uint8_t>> shippedBytes;
    if (!total.pristine.empty()) {
        const std::filesystem::path original = total.pristine / std::filesystem::relative(path, total.root);
        if (std::filesystem::is_regular_file(original)) {
            ts2::dbpf::Package shipped(original);
            for (const auto& entry : shipped.entries()) {
                shippedBytes[{entry.key.type, entry.key.group, entry.key.instance, entry.key.instanceHigh}] = shipped.readRaw(entry);
            }
        }
    }
    std::uint32_t lotVersion = 0;
    std::set<std::uint32_t> liveObjects;  // ids in the object map. Records of other ids are left from deleted objects.
    for (const ts2::dbpf::StoredResource& r : image.resources) {
        if (r.key.type == 0x4F626A4D && r.key.instance == 1) {
            try {
                const auto map = ts2::objm::parse(r.compressed ? ts2::qfs::decompress(r.bytes) : r.bytes);
                lotVersion = map.version;
                for (const auto& [id, type] : map.typeIndexByObject) {
                    liveObjects.insert(id);
                }
            } catch (const std::exception&) {
            }
        }
    }
    for (const ts2::dbpf::StoredResource& r : image.resources) {
        ++total.resources;
        std::vector<std::uint8_t> body = r.bytes;
        if (r.compressed) {
            body = ts2::qfs::decompress(r.bytes);
            if (ts2::qfs::compress(body) == r.bytes) {
                ++total.packedSame;
            } else {
                ++total.packedOther;
                ++packedOther;
            }
        }
        if (const auto it = shippedBytes.find({r.key.type, r.key.group, r.key.instance, r.key.instanceHigh});
            it != shippedBytes.end() && it->second == r.bytes) {
            ++total.shipped;
            bool same = false;
            bool skipped = false;
            if (const std::string kind = checkRecord(r.key.type, body, 0, same, skipped); !kind.empty()) {
                ++total.records[kind][3];
            }
            continue;
        }
        bool same = false;
        bool skipped = false;
        const bool objectRecord = r.key.type == 0x584F424A || r.key.type == 0x584D544F || r.key.type == 0x50455253 ||
                                  r.key.type == ts2::cobject::kType;
        const std::uint32_t version = objectRecord && !liveObjects.contains(r.key.instance) ? 0 : lotVersion;
        if (const std::string kind = checkRecord(r.key.type, body, version, same, skipped); !kind.empty()) {
            ++total.records[kind][skipped ? 2 : same ? 0 : 1];
            if (!skipped && !same) {
                std::printf("%s: %s %08X/%08X/%08X writes back differently\n", path.string().c_str(), kind.c_str(), r.key.type, r.key.group,
                            r.key.instance);
            }
        }
        const ts2::dbpf::StoredResource again = ts2::dbpf::storeResource(r.key, body);
        if (again.compressed == r.compressed && again.bytes == r.bytes) {
            ++total.ruleSame;
        } else {
            ++total.ruleOther;
            ++ruleOther;
        }
    }
    if (packedOther != 0 || ruleOther != 0) {
        std::printf("%s: %zu compressed differently, %zu stored differently\n", path.string().c_str(), packedOther, ruleOther);
    }
}

int saveCheck(const std::filesystem::path& root, const std::filesystem::path& pristine) {
    SaveCheck total;
    total.root = std::filesystem::is_directory(root) ? root : root.parent_path();
    total.pristine = pristine;
    if (std::filesystem::is_directory(root)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            std::string name = entry.path().filename().string();
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (entry.is_regular_file() && name.ends_with(".package")) {
                checkSavedPackage(entry.path(), total);
            }
        }
    } else {
        checkSavedPackage(root, total);
    }
    std::printf("%d packages: %d refused, %d written back the same, %d written back packed\n", total.files, total.refused,
                total.rewrittenSame, total.rewrittenOther);
    std::printf("%zu resources: compressor %zu same / %zu different, storing rule %zu same / %zu different, %zu as shipped\n",
                total.resources, total.packedSame, total.packedOther, total.ruleSame, total.ruleOther, total.shipped);
    bool recordsSame = true;
    for (const auto& [kind, counts] : total.records) {
        std::printf("%s records: %zu written back the same, %zu differently, %zu of other versions, %zu as shipped\n", kind.c_str(),
                    counts[0], counts[1], counts[2], counts[3]);
        recordsSame = recordsSame && counts[1] == 0;
    }
    std::printf("%zu of %zu Sims read with saved interactions that did not read\n", unreadSimStates, readSims);
    if (leftOverRecords != 0) {
        std::printf("%zu wants records keep bytes after their history (counted as other versions)\n", leftOverRecords);
    }
    return total.refused == 0 && total.packedOther == 0 && total.ruleOther == 0 && recordsSame ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string_view(argv[2]) == "--save-check") {
        try {
            return saveCheck(argv[1], argc > 4 && std::string_view(argv[3]) == "--pristine" ? std::filesystem::path(argv[4])
                                                                                           : std::filesystem::path());
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "error: %s\n", ex.what());
            return 1;
        }
    }
    if (argc < 2) {
        std::fprintf(stderr,
                     "Usage: dbpfdump <file.package> [--list | --verify | --grep <text> |\n"
                     "                                --str <group> <instance> | --str-survey | --ttab-survey | --lua-survey |\n"
                     "                                --ngbh | --datatable-survey | --datatables <name> |\n"
                     "                                --hex <type> <group> <instance> [bytes] |\n"
                     "                                --extract <type> <folder>]\n"
                     "       dbpfdump <file.package or folder> --save-check [--pristine <shipped folder>]\n");
        return 1;
    }
    const std::string_view mode = argc > 2 ? argv[2] : "";

    try {
        ts2::dbpf::Package package{std::filesystem::path(argv[1])};
        if (mode == "--grep" && argc > 3) {
            return grep(package, argv[3]);
        }
        if (mode == "--str" && argc > 4) {
            return showStrings(package, static_cast<std::uint32_t>(std::stoul(argv[3], nullptr, 0)),
                               static_cast<std::uint32_t>(std::stoul(argv[4], nullptr, 0)));
        }
        if (mode == "--hex" && argc > 5) {
            return hexDump(package, static_cast<std::uint32_t>(std::stoul(argv[3], nullptr, 0)),
                           static_cast<std::uint32_t>(std::stoul(argv[4], nullptr, 0)),
                           static_cast<std::uint32_t>(std::stoul(argv[5], nullptr, 0)),
                           argc > 6 ? std::stoul(argv[6], nullptr, 0) : 256);
        }
        if (mode == "--extract" && argc > 4) {
            return extract(package, static_cast<std::uint32_t>(std::stoul(argv[3], nullptr, 0)), argv[4]);
        }
        if (mode == "--lua-survey") {
            return surveyLuaScripts(package);
        }
        if (mode == "--ttab-survey") {
            return surveyInteractionTables(package);
        }
        if (mode == "--datatables" && argc > 3) {
            return printDataTables(package, argv[3]);
        }
        if (mode == "--datatable-survey") {
            return surveyDataTables(package);
        }
        if (mode == "--ngbh") {
            return showNeighborhoodRecord(package);
        }
        if (mode == "--str-survey") {
            return surveyStrings(package);
        }
        printHeader(package.header());
        if (mode == "--list") {
            printList(package);
        } else if (mode == "--verify") {
            return verifyDecompression(package);
        } else {
            printSummary(package);
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
    return 0;
}
