#include "formats/dbpf.hpp"
#include "formats/resource_types.hpp"
#include "formats/str.hpp"
#include "formats/luascript.hpp"
#include "formats/ngbh.hpp"
#include "formats/datatables.hpp"
#include "formats/ttab.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "Usage: dbpfdump <file.package> [--list | --verify | --grep <text> |\n"
                     "                                --str <group> <instance> | --str-survey | --ttab-survey | --lua-survey |\n"
                     "                                --ngbh | --datatable-survey |\n"
                     "                                --hex <type> <group> <instance> [bytes] |\n"
                     "                                --extract <type> <folder>]\n");
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
