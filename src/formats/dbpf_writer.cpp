#include "formats/dbpf_writer.hpp"

#include "formats/qfs.hpp"
#include "formats/resource_types.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ts2::dbpf {

namespace {

constexpr std::size_t kHeaderSize = 96;

// The compression directory's own key in packages the game writes.
constexpr ResourceKey kDirectoryKey{types::kDirectory, types::kDirectory, 0x286B1F03, 0};

void put(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void putKey(std::vector<std::uint8_t>& out, const ResourceKey& key, bool instanceHigh) {
    put(out, key.type);
    put(out, key.group);
    put(out, key.instance);
    if (instanceHigh) {
        put(out, key.instanceHigh);
    }
}

} // namespace

PackageWriter::PackageWriter(std::ostream& out) : out_(out) {
    const std::array<std::uint8_t, kHeaderSize> placeholder{};
    write(placeholder);
}

void PackageWriter::write(std::span<const std::uint8_t> bytes) {
    // Offsets in a package are 32 bits.
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max() - position_) {
        throw std::runtime_error("DBPF: the package would be larger than 4 GB");
    }
    if (!out_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("DBPF: writing the package failed");
    }
    position_ += bytes.size();
}

void PackageWriter::add(const ResourceKey& key, std::span<const std::uint8_t> stored, bool compressed, std::uint32_t uncompressedSize) {
    IndexEntry entry;
    entry.key = key;
    entry.offset = static_cast<std::uint32_t>(position_);
    entry.size = static_cast<std::uint32_t>(stored.size());
    entry.compressed = compressed;
    entry.uncompressedSize = compressed ? uncompressedSize : entry.size;
    write(stored);
    entries_.push_back(entry);
}

void PackageWriter::finish(std::uint32_t headerWord40) {
    bool instanceHigh = false;
    for (const IndexEntry& entry : entries_) {
        instanceHigh = instanceHigh || entry.key.instanceHigh != 0;
    }
    std::vector<std::uint8_t> directory;
    for (const IndexEntry& entry : entries_) {
        if (entry.compressed) {
            putKey(directory, entry.key, instanceHigh);
            put(directory, entry.uncompressedSize);
        }
    }
    if (!directory.empty()) {
        add(kDirectoryKey, directory, false, 0);
    }

    std::vector<std::uint8_t> index;
    for (const IndexEntry& entry : entries_) {
        putKey(index, entry.key, instanceHigh);
        put(index, entry.offset);
        put(index, entry.size);
    }
    const auto indexOffset = static_cast<std::uint32_t>(position_);
    write(index);

    std::vector<std::uint8_t> header{'D', 'B', 'P', 'F'};
    put(header, 1);  // version 1.2
    put(header, 2);
    header.resize(0x20, 0);  // reserved and dates, left 0 as the game leaves them
    put(header, 7);  // index version 7
    put(header, static_cast<std::uint32_t>(entries_.size()));
    put(header, indexOffset);
    put(header, static_cast<std::uint32_t>(index.size()));
    put(header, 0);  // no holes
    put(header, 0);
    put(header, 0);
    put(header, instanceHigh ? 2 : 1);
    put(header, headerWord40);
    header.resize(kHeaderSize, 0);
    if (!out_.seekp(0) || !out_.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size())) ||
        !out_.flush()) {
        throw std::runtime_error("DBPF: writing the package failed");
    }
}

} // namespace ts2::dbpf

namespace ts2::dbpf {

namespace {

// The game's limits for compressing a resource it writes, and the share of the plain size the
// compressed form must stay under to be kept.
constexpr std::size_t kCompressMin = 50;
constexpr std::size_t kCompressMax = 16000000;
constexpr float kCompressKeepBelow = 0.8f;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("DBPF: " + message);
}

std::uint32_t getU32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1]) << 8 |
           static_cast<std::uint32_t>(bytes[at + 2]) << 16 | static_cast<std::uint32_t>(bytes[at + 3]) << 24;
}

void setU32(std::array<std::uint8_t, kHeaderSize>& header, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        header[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

bool needsInstanceHigh(const PackageImage& image) {
    return std::any_of(image.resources.begin(), image.resources.end(),
                       [](const StoredResource& r) { return r.key.instanceHigh != 0; });
}

bool anyCompressed(const PackageImage& image) {
    return std::any_of(image.resources.begin(), image.resources.end(), [](const StoredResource& r) { return r.compressed; });
}

// The compression directory: the old directory's keys that are still compressed, in their order,
// then any other compressed resources in index order.
std::vector<std::uint8_t> directoryBytes(const PackageImage& image, bool instanceHigh) {
    std::unordered_map<ResourceKey, const StoredResource*, ResourceKeyHash> compressed;
    for (const StoredResource& r : image.resources) {
        if (r.compressed) {
            compressed[r.key] = &r;
        }
    }
    std::vector<std::uint8_t> out;
    std::unordered_set<ResourceKey, ResourceKeyHash> listed;
    auto list = [&](const StoredResource& r) {
        if (!listed.insert(r.key).second) {
            return;
        }
        putKey(out, r.key, instanceHigh);
        put(out, r.uncompressedSize);
    };
    for (const ResourceKey& key : image.directoryOrder) {
        if (const auto it = compressed.find(key); it != compressed.end()) {
            list(*it->second);
        }
    }
    for (const StoredResource& r : image.resources) {
        if (r.compressed) {
            list(r);
        }
    }
    return out;
}

void checkImage(const PackageImage& image) {
    const std::size_t count = image.resources.size();
    if (image.dataOrder.size() != count) {
        fail("the data order does not name every resource once");
    }
    std::vector<bool> seen(count, false);
    for (const std::size_t i : image.dataOrder) {
        if (i >= count || seen[i]) {
            fail("the data order does not name every resource once");
        }
        seen[i] = true;
    }
    if (image.directoryIndexPosition > count || image.directoryDataPosition > count) {
        fail("the compression directory's place lies past the resources");
    }
    std::unordered_set<ResourceKey, ResourceKeyHash> keys;
    for (const StoredResource& r : image.resources) {
        if (r.key.type == types::kDirectory) {
            fail("a resource has the compression directory's type");
        }
        if (!keys.insert(r.key).second) {
            fail("two resources have one key");
        }
        if (r.compressed && !qfs::isCompressed(r.bytes)) {
            fail("a resource marked compressed holds no compressed data");
        }
    }
}

} // namespace

StoredResource storeResource(const ResourceKey& key, std::span<const std::uint8_t> body) {
    StoredResource out;
    out.key = key;
    out.uncompressedSize = static_cast<std::uint32_t>(body.size());
    if (key.type != types::kDirectory && body.size() >= kCompressMin && body.size() <= kCompressMax) {
        std::vector<std::uint8_t> packed = qfs::compress(body);
        // The share is taken in single precision, as the game takes it, so a resource that packs to
        // exactly 0.8 of its size stays plain. Both sizes stay below 2^24 and are exact as floats.
        if (static_cast<float>(packed.size()) / static_cast<float>(body.size()) < kCompressKeepBelow) {
            out.bytes = std::move(packed);
            out.compressed = true;
            return out;
        }
    }
    out.bytes.assign(body.begin(), body.end());
    return out;
}

PackageImage readImage(std::span<const std::uint8_t> file) {
    Package package(file);
    if (package.appleDouble()) {
        fail("an AppleDouble file is not a package");
    }
    PackageImage image;
    std::copy_n(file.begin(), kHeaderSize, image.header.begin());

    const std::vector<IndexEntry>& entries = package.entries();
    std::vector<std::size_t> resourceOf(entries.size(), 0);
    bool haveDirectory = false;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const IndexEntry& entry = entries[i];
        if (entry.key.type == types::kDirectory) {
            if (haveDirectory) {
                fail("more than one compression directory");
            }
            haveDirectory = true;
            image.directoryIndexPosition = image.resources.size();
            const std::vector<std::uint8_t> bytes = package.readRaw(entry);
            const std::size_t recordSize = package.header().indexMinorVersion == 2 ? 20 : 16;
            for (std::size_t at = 0; at + recordSize <= bytes.size(); at += recordSize) {
                ResourceKey key{getU32(bytes, at), getU32(bytes, at + 4), getU32(bytes, at + 8), 0};
                if (recordSize == 20) {
                    key.instanceHigh = getU32(bytes, at + 12);
                }
                image.directoryOrder.push_back(key);
            }
            continue;
        }
        resourceOf[i] = image.resources.size();
        StoredResource r;
        r.key = entry.key;
        r.bytes = package.readRaw(entry);
        r.compressed = entry.compressed;
        r.uncompressedSize = entry.compressed ? entry.uncompressedSize : entry.size;
        // A compressed resource must unpack to its listed size, or a save would carry the damage on.
        if (r.compressed && (!qfs::isCompressed(r.bytes) || package.read(entry).size() != r.uncompressedSize)) {
            fail("a compressed resource does not unpack to its listed size");
        }
        image.resources.push_back(std::move(r));
    }
    if (!haveDirectory) {
        image.directoryIndexPosition = image.resources.size();
    }

    std::vector<std::size_t> byOffset(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        byOffset[i] = i;
    }
    // Empty resources share an offset with the next one. Taking them first at an offset reads a
    // written package back in the order it was written.
    std::stable_sort(byOffset.begin(), byOffset.end(), [&](std::size_t a, std::size_t b) {
        return entries[a].offset != entries[b].offset ? entries[a].offset < entries[b].offset : entries[a].size < entries[b].size;
    });
    image.directoryDataPosition = image.resources.size();
    for (const std::size_t i : byOffset) {
        if (entries[i].key.type == types::kDirectory) {
            image.directoryDataPosition = image.dataOrder.size();
        } else {
            image.dataOrder.push_back(resourceOf[i]);
        }
    }
    checkImage(image);
    return image;
}

std::vector<std::uint8_t> writeImage(const PackageImage& image) {
    checkImage(image);
    const bool instanceHigh = needsInstanceHigh(image);
    const bool withDirectory = anyCompressed(image);
    const std::vector<std::uint8_t> directory = withDirectory ? directoryBytes(image, instanceHigh) : std::vector<std::uint8_t>{};

    std::vector<std::uint8_t> out(kHeaderSize, 0);
    auto append = [&out](std::span<const std::uint8_t> bytes) -> std::uint32_t {
        // Offsets and sizes in a package are 32 bits.
        if (bytes.size() > std::numeric_limits<std::uint32_t>::max() - out.size()) {
            fail("the package would be larger than 4 GB");
        }
        const auto offset = static_cast<std::uint32_t>(out.size());
        out.insert(out.end(), bytes.begin(), bytes.end());
        return offset;
    };

    std::vector<std::uint32_t> offsets(image.resources.size(), 0);
    std::uint32_t directoryOffset = 0;
    for (std::size_t n = 0; n <= image.dataOrder.size(); ++n) {
        if (withDirectory && n == image.directoryDataPosition) {
            directoryOffset = append(directory);
        }
        if (n < image.dataOrder.size()) {
            const std::size_t i = image.dataOrder[n];
            offsets[i] = append(image.resources[i].bytes);
        }
    }

    std::vector<std::uint8_t> index;
    std::uint32_t entryCount = 0;
    for (std::size_t i = 0; i <= image.resources.size(); ++i) {
        if (withDirectory && i == image.directoryIndexPosition) {
            putKey(index, kDirectoryKey, instanceHigh);
            put(index, directoryOffset);
            put(index, static_cast<std::uint32_t>(directory.size()));
            ++entryCount;
        }
        if (i < image.resources.size()) {
            const StoredResource& r = image.resources[i];
            putKey(index, r.key, instanceHigh);
            put(index, offsets[i]);
            put(index, static_cast<std::uint32_t>(r.bytes.size()));
            ++entryCount;
        }
    }
    const std::uint32_t indexOffset = append(index);

    // The game rewrites its saves as version 1.2 with an index of version 7, and keeps the rest.
    std::array<std::uint8_t, kHeaderSize> header = image.header;
    header[0] = 'D';
    header[1] = 'B';
    header[2] = 'P';
    header[3] = 'F';
    setU32(header, 0x04, 1);
    setU32(header, 0x08, 2);
    setU32(header, 0x20, 7);
    setU32(header, 0x24, entryCount);
    setU32(header, 0x28, indexOffset);
    setU32(header, 0x2C, static_cast<std::uint32_t>(index.size()));
    setU32(header, 0x30, 0);
    setU32(header, 0x34, 0);
    setU32(header, 0x38, 0);
    setU32(header, 0x3C, instanceHigh ? 2 : 1);
    std::copy(header.begin(), header.end(), out.begin());
    return out;
}

void verifyImage(std::span<const std::uint8_t> file, const PackageImage& image) {
    Package package(file);
    const Header& h = package.header();
    if (package.appleDouble() || h.majorVersion != 1 || h.minorVersion != 2 || h.indexMajorVersion != 7 || h.holeCount != 0) {
        fail("check: the written header is not the one meant");
    }
    if (h.indexMinorVersion != (needsInstanceHigh(image) ? 2u : 1u)) {
        fail("check: the written index version is not the one meant");
    }
    const bool withDirectory = anyCompressed(image);
    const std::vector<IndexEntry>& entries = package.entries();
    if (entries.size() != image.resources.size() + (withDirectory ? 1 : 0)) {
        fail("check: the written package holds a different number of resources");
    }
    std::size_t next = 0;
    for (const IndexEntry& entry : entries) {
        if (entry.key.type == types::kDirectory) {
            if (!withDirectory) {
                fail("check: a compression directory was written where none was meant");
            }
            continue;
        }
        if (next >= image.resources.size()) {
            fail("check: the written index has an extra entry");
        }
        const StoredResource& r = image.resources[next++];
        if (!(entry.key == r.key) || entry.compressed != r.compressed) {
            fail("check: a written index entry differs from the one meant");
        }
        if (package.readRaw(entry) != r.bytes) {
            fail("check: a written resource's bytes differ from the ones meant");
        }
        if (r.compressed && package.read(entry).size() != r.uncompressedSize) {
            fail("check: a written resource does not unpack to its size");
        }
    }
    if (next != image.resources.size()) {
        fail("check: the written index lacks entries");
    }
}

} // namespace ts2::dbpf
