#include "content/resource_index.hpp"

#include <algorithm>

namespace ts2::content {

namespace {

// The C runtime allows 512 open files by default, and an install plus a neighborhood's character
// files is more packages than that.
constexpr std::size_t kMaxOpenPackages = 16;

} // namespace

std::size_t ResourceIndex::addPackage(const std::filesystem::path& path,
                                      std::optional<std::uint32_t> localGroup) {
    constexpr std::uint32_t kLocalGroup = 0xFFFFFFFF;
    auto package = std::make_unique<dbpf::Package>(path);
    const std::size_t packageIndex = packages_.size();

    byKey_.reserve(byKey_.size() + package->entries().size());
    for (dbpf::IndexEntry entry : package->entries()) {
        if (localGroup && entry.key.group == kLocalGroup) {
            entry.key.group = *localGroup;
        }
        ++stats_.entriesSeen;
        const auto [it, inserted] = byKey_.try_emplace(entry.key, resources_.size());
        if (inserted) {
            resources_.push_back({entry, packageIndex});
        } else {
            resources_[it->second] = {entry, packageIndex};
            ++stats_.overridden;
        }
    }

    packages_.push_back(std::move(package));
    paths_.push_back(path);
    return packageIndex;
}

const ResourceIndex::Resource* ResourceIndex::find(const dbpf::ResourceKey& key) const {
    const auto it = byKey_.find(key);
    return it != byKey_.end() ? &resources_[it->second] : nullptr;
}

std::vector<std::uint8_t> ResourceIndex::read(const Resource& resource) {
    return use(resource.package).read(resource.entry);
}

std::vector<std::uint8_t> ResourceIndex::readRaw(const Resource& resource) {
    return use(resource.package).readRaw(resource.entry);
}

dbpf::Package& ResourceIndex::use(std::size_t package) {
    if (const auto it = std::find(openPackages_.begin(), openPackages_.end(), package);
        it != openPackages_.end()) {
        openPackages_.erase(it);
    } else if (openPackages_.size() == kMaxOpenPackages) {
        packages_[openPackages_.front()]->close();
        openPackages_.erase(openPackages_.begin());
    }
    openPackages_.push_back(package);
    return *packages_[package];
}

} // namespace ts2::content
