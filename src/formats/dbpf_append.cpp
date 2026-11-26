#include "formats/dbpf_append.hpp"

#include "formats/resource_types.hpp"

#ifdef _WIN32
#include <share.h>
#endif

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ts2::dbpf {

namespace {

constexpr std::size_t kHeaderSize = 96;
// The header's counts and places of the index and the hole list.
constexpr std::size_t kIndexCountAt = 0x24, kHoleCountAt = 0x30;
// An index or hole list larger than this comes only from a broken file.
constexpr std::uint64_t kMostListBytes = 64ull << 20;

struct Region {
    std::uint32_t offset = 0, size = 0;
};

struct Entry {
    ResourceKey key;
    Region at;
};

std::uint32_t u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}

void put(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
    }
}

[[noreturn]] void fail(const std::filesystem::path& file, const std::string& why) {
    throw std::runtime_error("package " + file.filename().string() + " can't be added to: " + why);
}

std::vector<std::uint8_t> readAt(std::fstream& f, const std::filesystem::path& file, std::uint64_t offset, std::uint64_t size) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    f.seekg(static_cast<std::streamoff>(offset));
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) {
        fail(file, "it can't be read");
    }
    return bytes;
}

} // namespace

void appendToPackage(const std::filesystem::path& file, std::span<const StoredResource> added) {
    if (added.empty()) {
        return;
    }
    std::error_code ec;
    const std::uint64_t fileSize = std::filesystem::file_size(file, ec);
    if (ec || fileSize < kHeaderSize) {
        fail(file, "it is missing or shorter than a header");
    }
    for (std::size_t i = 0; i < added.size(); ++i) {
        for (std::size_t j = i + 1; j < added.size(); ++j) {
            if (added[i].key == added[j].key) {
                fail(file, "a resource is added twice");
            }
        }
    }
    // No other program may write the file meanwhile, as the game could while it runs.
#ifdef _WIN32
    std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary, _SH_DENYWR);
#else
    std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
#endif
    if (!f) {
        fail(file, "it can't be opened for writing");
    }
    const auto header = readAt(f, file, 0, kHeaderSize);
    if (header[0] != 'D' || header[1] != 'B' || header[2] != 'P' || header[3] != 'F' || u32(&header[4]) != 1) {
        fail(file, "it is not a version 1 package");
    }
    const std::uint32_t indexMajor = u32(&header[0x20]), indexMinor = u32(&header[0x3C]);
    if (indexMajor != 7 || (indexMinor != 1 && indexMinor != 2)) {
        fail(file, "its index is not version 7.1 or 7.2");
    }
    const bool high = indexMinor == 2;
    const std::uint32_t entrySize = high ? 24 : 20, recordSize = high ? 20 : 16;
    const std::uint32_t count = u32(&header[kIndexCountAt]), indexOffset = u32(&header[0x28]), indexSize = u32(&header[0x2C]);
    const std::uint32_t holeCount = u32(&header[kHoleCountAt]), holeOffset = u32(&header[0x34]), holeSize = u32(&header[0x38]);
    const auto inside = [&](std::uint64_t offset, std::uint64_t size) { return offset + size <= fileSize; };
    if (std::uint64_t{count} * entrySize > kMostListBytes || std::uint64_t{holeCount} * 8 > kMostListBytes ||
        !inside(indexOffset, std::uint64_t{count} * entrySize) || (holeCount != 0 && !inside(holeOffset, std::uint64_t{holeCount} * 8))) {
        fail(file, "its index or hole list is broken");
    }
    for (const auto& r : added) {
        if (!high && r.key.instanceHigh != 0) {
            fail(file, "its index has no instance-high fields");
        }
    }

    std::vector<Entry> entries;
    entries.reserve(count);
    const auto index = readAt(f, file, indexOffset, std::uint64_t{count} * entrySize);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* p = &index[std::size_t{i} * entrySize];
        Entry e;
        e.key = {u32(p), u32(p + 4), u32(p + 8), high ? u32(p + 12) : 0};
        e.at = {u32(p + entrySize - 8), u32(p + entrySize - 4)};
        if (!inside(e.at.offset, e.at.size)) {
            fail(file, "a resource lies outside the file");
        }
        entries.push_back(e);
    }
    std::vector<Region> holes;
    if (holeCount != 0) {
        const auto list = readAt(f, file, holeOffset, std::uint64_t{holeCount} * 8);
        for (std::uint32_t i = 0; i < holeCount; ++i) {
            holes.push_back({u32(&list[std::size_t{i} * 8]), u32(&list[std::size_t{i} * 8 + 4])});
        }
    }

    // The compression directory: the key and unpacked size of each compressed resource.
    struct Record {
        ResourceKey key;
        std::uint32_t size = 0;
    };
    std::vector<Record> records;
    const auto dir = std::find_if(entries.begin(), entries.end(), [](const Entry& e) { return e.key.type == types::kDirectory; });
    std::optional<Entry> oldDir;
    if (dir != entries.end()) {
        if (dir->at.size > kMostListBytes) {
            fail(file, "its compression directory is broken");
        }
        oldDir = *dir;
        const auto bytes = readAt(f, file, dir->at.offset, dir->at.size);
        for (std::size_t p = 0; p + recordSize <= bytes.size(); p += recordSize) {
            records.push_back({{u32(&bytes[p]), u32(&bytes[p + 4]), u32(&bytes[p + 8]), high ? u32(&bytes[p + 12]) : 0},
                               u32(&bytes[p + recordSize - 4])});
        }
    }
    bool dirChanged = false;
    for (const auto& r : added) {
        if (r.key.type == types::kDirectory) {
            fail(file, "the compression directory can't be added as a resource");
        }
        const auto before = records.size();
        std::erase_if(records, [&](const Record& d) { return d.key == r.key; });
        dirChanged = dirChanged || records.size() != before;
        if (r.compressed) {
            records.push_back({r.key, r.uncompressedSize});
            dirChanged = true;
        }
        std::erase_if(entries, [&](const Entry& e) {
            if (e.key == r.key) {
                holes.push_back(e.at);
                return true;
            }
            return false;
        });
    }

    // Everything new goes after the file's end: the resources, the directory, the index, the holes.
    std::uint64_t end = fileSize;
    std::vector<std::uint8_t> tail;
    const auto place = [&](std::size_t size) {
        if (end + tail.size() + size > std::numeric_limits<std::uint32_t>::max()) {
            fail(file, "it would grow past 4 GB");
        }
        return Region{static_cast<std::uint32_t>(end + tail.size()), static_cast<std::uint32_t>(size)};
    };
    for (const auto& r : added) {
        entries.push_back({r.key, place(r.bytes.size())});
        tail.insert(tail.end(), r.bytes.begin(), r.bytes.end());
    }
    if (dirChanged) {
        if (oldDir) {
            holes.push_back(oldDir->at);
            std::erase_if(entries, [](const Entry& e) { return e.key.type == types::kDirectory; });
        }
        if (!records.empty()) {
            std::vector<std::uint8_t> bytes;
            for (const auto& d : records) {
                put(bytes, d.key.type);
                put(bytes, d.key.group);
                put(bytes, d.key.instance);
                if (high) {
                    put(bytes, d.key.instanceHigh);
                }
                put(bytes, d.size);
            }
            const ResourceKey key = oldDir ? oldDir->key : ResourceKey{types::kDirectory, types::kDirectory, 0x286B1F03, 0};
            entries.push_back({key, place(bytes.size())});
            tail.insert(tail.end(), bytes.begin(), bytes.end());
        }
    }
    // The old lists become holes by the header's sizes, as the game frees them.
    holes.push_back({indexOffset, indexSize});
    if (holeCount != 0) {
        holes.push_back({holeOffset, holeSize});
    }
    std::erase_if(holes, [](const Region& h) { return h.size == 0; });
    std::vector<std::uint8_t> newIndex;
    for (const auto& e : entries) {
        put(newIndex, e.key.type);
        put(newIndex, e.key.group);
        put(newIndex, e.key.instance);
        if (high) {
            put(newIndex, e.key.instanceHigh);
        }
        put(newIndex, e.at.offset);
        put(newIndex, e.at.size);
    }
    const Region indexAt = place(newIndex.size());
    tail.insert(tail.end(), newIndex.begin(), newIndex.end());
    std::vector<std::uint8_t> newHoles;
    for (const auto& h : holes) {
        put(newHoles, h.offset);
        put(newHoles, h.size);
    }
    const Region holesAt = place(newHoles.size());
    tail.insert(tail.end(), newHoles.begin(), newHoles.end());

    f.seekp(static_cast<std::streamoff>(end));
    f.write(reinterpret_cast<const char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    f.flush();
    if (!f) {
        f.close();
        std::filesystem::resize_file(file, fileSize, ec);
        fail(file, "its new resources can't be written");
    }
    // The header last: until it is written the file reads as it did.
    std::vector<std::uint8_t> fields;
    put(fields, static_cast<std::uint32_t>(entries.size()));
    put(fields, indexAt.offset);
    put(fields, indexAt.size);
    put(fields, static_cast<std::uint32_t>(holes.size()));
    put(fields, holes.empty() ? 0 : holesAt.offset);
    put(fields, holesAt.size);
    f.seekp(static_cast<std::streamoff>(kIndexCountAt));
    f.write(reinterpret_cast<const char*>(fields.data()), static_cast<std::streamsize>(fields.size()));
    f.flush();
    if (!f) {
        fail(file, "its header can't be written");
    }
}

} // namespace ts2::dbpf
