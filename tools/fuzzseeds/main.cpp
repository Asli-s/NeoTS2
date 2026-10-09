#include "formats/dbpf.hpp"
#include "formats/qfs.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxSeedSize = 4096;
constexpr std::size_t kQfsSeedCount = 40;
constexpr std::size_t kPackageResourceCount = 8;
constexpr std::uint32_t kDirectoryType = 0xE86B1EEF;
constexpr std::uint32_t kBehaviorType = 0x42484156;

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void writeFile(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

struct Resource {
    ts2::dbpf::IndexEntry entry;
    std::vector<std::uint8_t> raw;
};

std::vector<std::uint8_t> buildPackage(const std::vector<Resource>& resources) {
    std::vector<std::uint8_t> out(96, 0);

    std::vector<ts2::dbpf::IndexEntry> index;
    std::vector<std::uint8_t> dir;
    for (const auto& r : resources) {
        ts2::dbpf::IndexEntry e = r.entry;
        e.offset = static_cast<std::uint32_t>(out.size());
        e.size = static_cast<std::uint32_t>(r.raw.size());
        out.insert(out.end(), r.raw.begin(), r.raw.end());
        index.push_back(e);

        put32(dir, e.key.type);
        put32(dir, e.key.group);
        put32(dir, e.key.instance);
        put32(dir, e.key.instanceHigh);
        put32(dir, r.entry.uncompressedSize);
    }

    ts2::dbpf::IndexEntry dirEntry;
    dirEntry.key = {kDirectoryType, kDirectoryType, 0x286B1F03, 0};
    dirEntry.offset = static_cast<std::uint32_t>(out.size());
    dirEntry.size = static_cast<std::uint32_t>(dir.size());
    out.insert(out.end(), dir.begin(), dir.end());
    index.push_back(dirEntry);

    const auto indexOffset = static_cast<std::uint32_t>(out.size());
    for (const auto& e : index) {
        put32(out, e.key.type);
        put32(out, e.key.group);
        put32(out, e.key.instance);
        put32(out, e.key.instanceHigh);
        put32(out, e.offset);
        put32(out, e.size);
    }
    const auto indexSize = static_cast<std::uint32_t>(out.size() - indexOffset);

    std::vector<std::uint8_t> header;
    header.insert(header.end(), {'D', 'B', 'P', 'F'});
    put32(header, 1);
    put32(header, 1);
    header.resize(32, 0);
    put32(header, 7);
    put32(header, static_cast<std::uint32_t>(index.size()));
    put32(header, indexOffset);
    put32(header, indexSize);
    put32(header, 0);
    put32(header, 0);
    put32(header, 0);
    put32(header, 2);
    std::copy(header.begin(), header.end(), out.begin());
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: fuzzseeds <source.package> <corpus-dir>\n");
        return 1;
    }

    try {
        ts2::dbpf::Package package{fs::path(argv[1])};
        const fs::path qfsDir = fs::path(argv[2]) / "qfs";
        const fs::path dbpfDir = fs::path(argv[2]) / "dbpf";
        const fs::path bhavDir = fs::path(argv[2]) / "bhav";
        fs::create_directories(qfsDir);
        fs::create_directories(dbpfDir);
        fs::create_directories(bhavDir);

        std::map<std::uint16_t, std::size_t> bhavPerFormat;
        for (const auto& e : package.entries()) {
            if (e.key.type != kBehaviorType || e.uncompressedSize > kMaxSeedSize) {
                continue;
            }
            const std::vector<std::uint8_t> data = package.read(e);
            if (data.size() < 66) {
                continue;
            }
            const auto format = static_cast<std::uint16_t>(data[64] | data[65] << 8);
            std::size_t& n = bhavPerFormat[format];
            if (n < 3) {
                writeFile(bhavDir / ("seed_" + std::to_string(format) + "_" + std::to_string(n) + ".bin"),
                          data);
                ++n;
            }
        }

        std::vector<Resource> picked;
        std::set<std::uint32_t> typesSeen;
        for (const auto& e : package.entries()) {
            if (!e.compressed || e.size < 32 || e.size > kMaxSeedSize) {
                continue;
            }
            const bool newType = typesSeen.insert(e.key.type).second;
            if (!newType && picked.size() >= kQfsSeedCount / 2) {
                continue;
            }
            std::vector<std::uint8_t> raw = package.readRaw(e);
            if (!ts2::qfs::isCompressed(raw)) {
                continue;
            }
            picked.push_back({e, std::move(raw)});
            if (picked.size() == kQfsSeedCount) {
                break;
            }
        }

        for (std::size_t i = 0; i < picked.size(); ++i) {
            writeFile(qfsDir / ("seed_" + std::to_string(i) + ".bin"), picked[i].raw);
        }

        const std::vector<Resource> subset(
            picked.begin(), picked.begin() + std::min(picked.size(), kPackageResourceCount));
        const std::vector<std::uint8_t> mini = buildPackage(subset);
        writeFile(dbpfDir / "mini.package", mini);

        ts2::dbpf::Package check{std::span<const std::uint8_t>(mini)};
        std::size_t decoded = 0;
        for (const auto& e : check.entries()) {
            if (e.compressed && check.read(e).size() == e.uncompressedSize) {
                ++decoded;
            }
        }

        std::printf("Wrote %zu QFS seeds and a %zu-byte package (%zu/%zu resources round-trip)\n",
                    picked.size(), mini.size(), decoded, subset.size());
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
    return 0;
}
