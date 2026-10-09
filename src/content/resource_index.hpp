#pragma once

#include "formats/dbpf.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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

    // Resources in group 0xFFFFFFFF belong to the package itself (character files use it); with
    // `localGroup` they are indexed under that group instead.
    std::size_t addPackage(const std::filesystem::path& path,
                           std::optional<std::uint32_t> localGroup = std::nullopt);

    const std::vector<Resource>& resources() const { return resources_; }
    const Resource* find(const dbpf::ResourceKey& key) const;
    std::vector<std::uint8_t> read(const Resource& resource);
    std::vector<std::uint8_t> readRaw(const Resource& resource);

    std::size_t packageCount() const { return packages_.size(); }
    const std::filesystem::path& packagePath(std::size_t package) const { return paths_[package]; }
    const Stats& stats() const { return stats_; }

private:
    dbpf::Package& use(std::size_t package);

    std::vector<std::unique_ptr<dbpf::Package>> packages_;
    // Packages most recently read from, oldest first; only these keep their files open.
    std::vector<std::size_t> openPackages_;
    std::vector<std::filesystem::path> paths_;
    std::vector<Resource> resources_;
    std::unordered_map<dbpf::ResourceKey, std::size_t, dbpf::ResourceKeyHash> byKey_;
    Stats stats_;
};

} // namespace ts2::content
