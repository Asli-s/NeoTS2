#pragma once

#include "formats/dbpf.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace ts2::content {

class ResourceIndex {
public:
    struct Resource {
        dbpf::IndexEntry entry;
        std::size_t package = 0;
    };

    struct Stats {
        std::size_t entriesSeen = 0;
        std::size_t overridden = 0;
    };

    // Every resource of an index in a stable order, each once. Valid until something is added.
    class ResourceList {
    public:
        class iterator {
        public:
            explicit iterator(const Resource* const* at) : at_(at) {}
            const Resource& operator*() const { return **at_; }
            iterator& operator++() {
                ++at_;
                return *this;
            }
            bool operator==(const iterator&) const = default;

        private:
            const Resource* const* at_;
        };
        iterator begin() const { return iterator(items.data()); }
        iterator end() const { return iterator(items.data() + items.size()); }
        std::size_t size() const { return items.size(); }
        bool empty() const { return items.empty(); }
        const Resource& operator[](std::size_t i) const { return *items[i]; }

        std::vector<const Resource*> items;
    };

    // Packages several indexes take alike, kept once, as a real mod collection holds hundreds of
    // thousands of resources. Read only once given to an index, so threads may share it.
    class SharedPackages {
    public:
        // Later packages override earlier ones. Resources in group 0xFFFFFFFF are filed under `localGroup`.
        void add(const std::filesystem::path& path, std::uintmax_t fileSize, std::span<const dbpf::IndexEntry> entries, std::uint32_t localGroup);
        std::size_t packageCount() const { return paths_.size(); }

    private:
        friend class ResourceIndex;
        std::vector<std::filesystem::path> paths_;
        std::vector<std::uintmax_t> sizes_;
        std::vector<Resource> resources_;
        std::unordered_map<dbpf::ResourceKey, std::size_t, dbpf::ResourceKeyHash> byKey_;
        Stats stats_;
    };

    // Resources in group 0xFFFFFFFF belong to the package itself (character files use it). With
    // `localGroup` they are indexed under that group instead. False when the file is an AppleDouble
    // sidecar, which is left out.
    bool addPackage(const std::filesystem::path& path,
                    std::optional<std::uint32_t> localGroup = std::nullopt);

    // The same for a package whose index was read before, which opens no file. Its resources in
    // group 0xFFFFFFFF are indexed under `localGroup`.
    void addIndexed(const std::filesystem::path& path, std::uintmax_t fileSize, std::span<const dbpf::IndexEntry> entries, std::uint32_t localGroup);

    // Adds these packages after what the index holds, so they override it, and what is added later
    // overrides them. An index takes one such set, and a later set replaces it.
    void addShared(std::shared_ptr<const SharedPackages> packages);

    const ResourceList& resources() const;
    const Resource* find(const dbpf::ResourceKey& key) const;
    std::vector<std::uint8_t> read(const Resource& resource);
    std::vector<std::uint8_t> readRaw(const Resource& resource);

    std::size_t packageCount() const { return packages_.size() + (shared_ ? shared_->packageCount() : 0); }
    const std::filesystem::path& packagePath(std::size_t package) const;
    const Stats& stats() const { return stats_; }

private:
    dbpf::Package& use(std::size_t package);
    dbpf::Package& packageAt(std::size_t package);
    void addEntries(std::span<const dbpf::IndexEntry> entries, std::optional<std::uint32_t> localGroup);

    std::vector<std::unique_ptr<dbpf::Package>> packages_;
    // Packages most recently read from, oldest first. Only these keep their files open.
    std::vector<std::size_t> openPackages_;
    std::vector<std::filesystem::path> paths_;
    std::vector<Resource> resources_;
    std::unordered_map<dbpf::ResourceKey, std::size_t, dbpf::ResourceKeyHash> byKey_;
    Stats stats_;
    std::shared_ptr<const SharedPackages> shared_;
    // This index's own resources from here on were added after the shared ones.
    std::size_t sharedAt_ = 0;
    // A resource from this package on overrides the shared ones, even where it took an older one's place.
    std::size_t firstPackageAfterShared_ = 0;
    // The shared packages' files, opened by this index alone, each made when first read.
    std::vector<std::unique_ptr<dbpf::Package>> sharedFiles_;
    // Listed when first asked for. A pointer so that an index can still be moved.
    mutable std::unique_ptr<std::mutex> listLock_ = std::make_unique<std::mutex>();
    mutable ResourceList list_;
    mutable bool listed_ = false;
};

} // namespace ts2::content
