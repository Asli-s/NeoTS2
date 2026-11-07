#include "content/resource_index.hpp"

#include "diag/log.hpp"

#include <algorithm>
#include <limits>

namespace ts2::content {

namespace {

// The C runtime allows 512 open files by default, and an install plus a neighborhood's character
// files is more packages than that.
constexpr std::size_t kMaxOpenPackages = 16;

// Marks a resource's package number as one of the shared packages'.
constexpr std::size_t kSharedPackage = (std::numeric_limits<std::size_t>::max() >> 1) + 1;

constexpr std::uint32_t kLocalGroup = 0xFFFFFFFF;

} // namespace

void ResourceIndex::SharedPackages::add(const std::filesystem::path& path, std::uintmax_t fileSize, std::span<const dbpf::IndexEntry> entries, std::uint32_t localGroup) {
    const std::size_t package = kSharedPackage | paths_.size();
    byKey_.reserve(byKey_.size() + entries.size());
    for (dbpf::IndexEntry entry : entries) {
        if (entry.key.group == kLocalGroup) {
            entry.key.group = localGroup;
        }
        ++stats_.entriesSeen;
        const auto [it, inserted] = byKey_.try_emplace(entry.key, resources_.size());
        if (inserted) {
            resources_.push_back({entry, package});
        } else {
            resources_[it->second] = {entry, package};
            ++stats_.overridden;
        }
    }
    paths_.push_back(path);
    sizes_.push_back(fileSize);
}

bool ResourceIndex::addPackage(const std::filesystem::path& path,
                               std::optional<std::uint32_t> localGroup) {
    auto package = std::make_unique<dbpf::Package>(path);
    if (package->appleDouble()) {
        diag::warnOnce("packages: AppleDouble metadata is left out: " + diag::utf8(path.filename()));
        return false;
    }
    addEntries(package->entries(), localGroup);
    // The index now holds them, and reading a resource takes its entry from there.
    package->forgetEntries();
    packages_.push_back(std::move(package));
    paths_.push_back(path);
    return true;
}

void ResourceIndex::addIndexed(const std::filesystem::path& path, std::uintmax_t fileSize, std::span<const dbpf::IndexEntry> entries, std::uint32_t localGroup) {
    addEntries(entries, localGroup);
    packages_.push_back(std::make_unique<dbpf::Package>(path, fileSize));
    paths_.push_back(path);
}

// Call before adding the package to packages_, whose new place the resources name.
void ResourceIndex::addEntries(std::span<const dbpf::IndexEntry> entries, std::optional<std::uint32_t> localGroup) {
    const std::size_t packageIndex = packages_.size();
    byKey_.reserve(byKey_.size() + entries.size());
    for (dbpf::IndexEntry entry : entries) {
        if (localGroup && entry.key.group == kLocalGroup) {
            entry.key.group = *localGroup;
        }
        ++stats_.entriesSeen;
        const auto [it, inserted] = byKey_.try_emplace(entry.key, resources_.size());
        if (inserted) {
            resources_.push_back({entry, packageIndex});
            stats_.overridden += shared_ && shared_->byKey_.contains(entry.key) ? 1 : 0;
        } else {
            resources_[it->second] = {entry, packageIndex};
            ++stats_.overridden;
        }
    }
    const std::lock_guard lock(*listLock_);
    listed_ = false;
}

void ResourceIndex::addShared(std::shared_ptr<const SharedPackages> packages) {
    for (std::size_t package : openPackages_) {
        if ((package & kSharedPackage) != 0) {
            sharedFiles_[package & ~kSharedPackage]->close();
        }
    }
    std::erase_if(openPackages_, [](std::size_t package) { return (package & kSharedPackage) != 0; });
    sharedFiles_.clear();
    shared_ = std::move(packages);
    sharedAt_ = resources_.size();
    firstPackageAfterShared_ = packages_.size();
    if (shared_) {
        sharedFiles_.resize(shared_->packageCount());
        stats_.entriesSeen += shared_->stats_.entriesSeen;
        stats_.overridden += shared_->stats_.overridden;
        for (const Resource& own : resources_) {
            stats_.overridden += shared_->byKey_.contains(own.entry.key) ? 1 : 0;
        }
    }
    const std::lock_guard lock(*listLock_);
    listed_ = false;
}

// The order the resources would have if the shared packages were added to the index one by one: a
// resource overriding an earlier one takes its place, and new ones follow.
const ResourceIndex::ResourceList& ResourceIndex::resources() const {
    const std::lock_guard lock(*listLock_);
    if (listed_) {
        return list_;
    }
    auto& items = list_.items;
    items.clear();
    if (!shared_) {
        items.reserve(resources_.size());
        for (const Resource& r : resources_) {
            items.push_back(&r);
        }
    } else {
        items.reserve(resources_.size() + shared_->resources_.size());
        for (std::size_t i = 0; i < sharedAt_; ++i) {
            const auto over = shared_->byKey_.find(resources_[i].entry.key);
            const bool sharedWins = over != shared_->byKey_.end() && resources_[i].package < firstPackageAfterShared_;
            items.push_back(sharedWins ? &shared_->resources_[over->second] : &resources_[i]);
        }
        for (const Resource& r : shared_->resources_) {
            const auto own = byKey_.find(r.entry.key);
            if (own == byKey_.end()) {
                items.push_back(&r);
            } else if (own->second >= sharedAt_) {
                items.push_back(&resources_[own->second]);
            }
        }
        for (std::size_t i = sharedAt_; i < resources_.size(); ++i) {
            if (!shared_->byKey_.contains(resources_[i].entry.key)) {
                items.push_back(&resources_[i]);
            }
        }
    }
    listed_ = true;
    return list_;
}

const ResourceIndex::Resource* ResourceIndex::find(const dbpf::ResourceKey& key) const {
    const auto it = byKey_.find(key);
    if (shared_ && (it == byKey_.end() || resources_[it->second].package < firstPackageAfterShared_)) {
        if (const auto over = shared_->byKey_.find(key); over != shared_->byKey_.end()) {
            return &shared_->resources_[over->second];
        }
    }
    return it != byKey_.end() ? &resources_[it->second] : nullptr;
}

std::vector<std::uint8_t> ResourceIndex::read(const Resource& resource) {
    return use(resource.package).read(resource.entry);
}

std::vector<std::uint8_t> ResourceIndex::readRaw(const Resource& resource) {
    return use(resource.package).readRaw(resource.entry);
}

const std::filesystem::path& ResourceIndex::packagePath(std::size_t package) const {
    if ((package & kSharedPackage) != 0) {
        return shared_->paths_[package & ~kSharedPackage];
    }
    return package < paths_.size() ? paths_[package] : shared_->paths_[package - paths_.size()];
}

dbpf::Package& ResourceIndex::packageAt(std::size_t package) {
    if ((package & kSharedPackage) == 0) {
        return *packages_[package];
    }
    const std::size_t at = package & ~kSharedPackage;
    if (!sharedFiles_[at]) {
        sharedFiles_[at] = std::make_unique<dbpf::Package>(shared_->paths_[at], shared_->sizes_[at]);
    }
    return *sharedFiles_[at];
}

dbpf::Package& ResourceIndex::use(std::size_t package) {
    if (const auto it = std::find(openPackages_.begin(), openPackages_.end(), package);
        it != openPackages_.end()) {
        openPackages_.erase(it);
    } else if (openPackages_.size() == kMaxOpenPackages) {
        packageAt(openPackages_.front()).close();
        openPackages_.erase(openPackages_.begin());
    }
    openPackages_.push_back(package);
    return packageAt(package);
}

} // namespace ts2::content
