#include "content/thumbnail_merge.hpp"

#include "formats/dbpf.hpp"
#include "formats/dbpf_writer.hpp"
#include "formats/resource_types.hpp"

#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace ts2::content {

namespace {

// The packs ship about 32,000 thumbnails in all. The limit stops a broken or tampered install from
// making a package without end.
constexpr std::size_t kMaxResources = 500000;
// A shipped thumbnail has one high instance per type and instance. The limit keeps the replace check
// short in a tampered package.
constexpr std::size_t kMaxSameInstance = 64;

constexpr std::uint32_t kUserGroup = 0xFFFFFFFF;

// The game writes this in the header's 4 bytes at 0x40 of the packages it merges. Shipped packages
// have a pack's own number there. What it means is not known yet.
constexpr std::uint32_t kHeaderWord40 = 0x00010000;

struct Kept {
    std::size_t package;
    std::size_t entry;
    dbpf::ResourceKey key;
    bool replaced = false;
};

bool sameThumbnail(const dbpf::ResourceKey& a, const dbpf::ResourceKey& b) {
    return a.instanceHigh == b.instanceHigh || a.instanceHigh == 0 || b.instanceHigh == 0;
}

} // namespace

std::uintmax_t mergeThumbnailPackages(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& target,
                                      std::uintmax_t maxBytes) {
    std::vector<dbpf::Package> packages;
    packages.reserve(sources.size());
    std::vector<Kept> kept;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> byTypeAndInstance;
    for (const auto& source : sources) {
        const dbpf::Package& package = packages.emplace_back(source);
        const auto& entries = package.entries();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].key.type == types::kDirectory) {
                continue;
            }
            dbpf::ResourceKey key = entries[i].key;
            key.group = kUserGroup;
            auto& same = byTypeAndInstance[(std::uint64_t{key.type} << 32) | key.instance];
            std::erase_if(same, [&](std::size_t k) {
                kept[k].replaced = kept[k].replaced || sameThumbnail(kept[k].key, key);
                return kept[k].replaced;
            });
            if (kept.size() >= kMaxResources || same.size() >= kMaxSameInstance) {
                throw std::runtime_error("more thumbnails than any install ships");
            }
            same.push_back(kept.size());
            kept.push_back({packages.size() - 1, i, key});
        }
    }

    std::uintmax_t size = 0;
    for (const Kept& k : kept) {
        size += k.replaced ? 0 : packages[k.package].entries()[k.entry].size;
    }
    if (size > maxBytes) {
        throw std::runtime_error("the thumbnails are larger than any install ships");
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("can't create the package");
    }
    dbpf::PackageWriter writer(out);
    for (const Kept& k : kept) {
        if (!k.replaced) {
            const dbpf::IndexEntry& entry = packages[k.package].entries()[k.entry];
            writer.add(k.key, packages[k.package].readRaw(entry), entry.compressed, entry.uncompressedSize);
        }
    }
    writer.finish(kHeaderWord40);
    out.close();
    for (dbpf::Package& package : packages) {
        package.close();
    }
    if (!out) {
        throw std::runtime_error("writing the package failed");
    }
    return std::filesystem::file_size(target);
}

} // namespace ts2::content
