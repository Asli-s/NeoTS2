#include "audio/bank.hpp"

#include "content/install.hpp"
#include "diag/log.hpp"
#include "formats/hash.hpp"
#include "formats/propset.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>

namespace ts2::audio {

namespace {

constexpr std::uint32_t kTrackSettings = 0x0B9EB87E;
constexpr std::uint32_t kHitList = 0x7B1ACFCD;
constexpr std::uint32_t kAudio = 0x2026960B;
constexpr std::uint32_t kPropertySet = 0xEBFEE33F;
// The track settings key that marks a looping track.
constexpr std::string_view kLoopKey = "0xff595ffd";

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

// The product folder's TSData: the parent chain of an object package up to the TSData folder.
std::filesystem::path dataFolder(const content::Product& product) {
    for (const auto& package : product.objectPackages) {
        std::filesystem::path folder = package.parent_path();
        while (folder.has_relative_path()) {
            if (lower(folder.filename().string()) == "tsdata") {
                return folder;
            }
            folder = folder.parent_path();
        }
    }
    return {};
}

} // namespace

void Bank::addInstall(const std::filesystem::path& installRoot) {
    for (const auto& product : content::findProducts(installRoot)) {
        const std::filesystem::path data = dataFolder(product);
        if (data.empty()) {
            continue;
        }
        addPackages(data / "Res" / "Sound");
        std::error_code error;
        for (const auto& locale : std::filesystem::directory_iterator(data / "Res" / "Locale", error)) {
            if (locale.is_directory()) {
                addPackages(locale.path() / "Sound");
            }
        }
    }
    indexAll();
}

void Bank::addPackages(const std::filesystem::path& folder) {
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (const auto& item : std::filesystem::directory_iterator(folder, error)) {
        if (item.is_regular_file() && lower(item.path().extension().string()) == ".package") {
            files.push_back(item.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        const std::string key = lower(file.string());
        if (!packages_.insert(key).second) {
            continue;
        }
        try {
            index_.addPackage(file);
        } catch (const std::exception& ex) {
            ts2::diag::warnOnce("sound package " + file.filename().string() + ": " + ex.what());
        }
    }
}

void Bank::indexAll() {
    for (const auto& resource : index_.resources()) {
        const auto type = resource.entry.key.type;
        auto* map = type == kTrackSettings  ? &tracks_
                    : type == kHitList      ? &hitLists_
                    : type == kAudio        ? &samples_
                    : type == kPropertySet  ? &propertySets_
                                            : nullptr;
        if (map != nullptr) {
            (*map)[keyOf(resource.entry.key.instance, resource.entry.key.instanceHigh)].push_back(&resource);
        }
    }
}

const content::ResourceIndex::Resource* Bank::find(const std::map<Key, std::vector<const content::ResourceIndex::Resource*>>& map,
                                    std::string_view trackName) const {
    const auto key = ts2::hash::scenegraphKey(trackName);
    if (const auto it = map.find(keyOf(key.instance, key.instanceHigh)); it != map.end()) {
        return it->second.front();
    }
    if (key.instanceHigh != 0) {
        if (const auto it = map.find(keyOf(key.instance, 0)); it != map.end()) {
            return it->second.front();
        }
    }
    return nullptr;
}

bool Bank::hasTrack(std::string_view trackName) const {
    return find(tracks_, trackName) != nullptr;
}

std::vector<SampleLocation> Bank::samplesOf(std::string_view trackName) {
    const content::ResourceIndex::Resource* track = find(tracks_, trackName);
    const content::ResourceIndex::Resource* list = find(hitLists_, trackName);
    if (track == nullptr || list == nullptr) {
        return {};
    }
    std::vector<SampleLocation> out;
    const auto bytes = index_.read(*list);
    if (bytes.size() < 8) {
        return out;
    }
    const std::uint32_t count = le32(&bytes[4]);
    // Each entry is two u32 keys; a count past the resource is a broken file, not a lot of entries.
    if (count > (bytes.size() - 8) / 8) {
        return out;
    }
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* entry = &bytes[8 + static_cast<std::size_t>(i) * 8];
        out.push_back({0, le32(entry), le32(entry + 4)});
    }
    return out;
}

bool Bank::loops(std::string_view trackName) {
    const std::string low = lower(trackName);
    if (low.ends_with("_lp")) {
        return true;
    }
    const content::ResourceIndex::Resource* track = find(tracks_, trackName);
    if (track == nullptr) {
        return false;
    }
    try {
        const auto properties = ts2::propset::parse(index_.read(*track));
        const auto it = properties.find(std::string(kLoopKey));
        return it != properties.end() && it->second != "0" && !it->second.empty();
    } catch (const std::exception&) {
        return false;
    }
}

const content::ResourceIndex::Resource* Bank::findSample(const SampleLocation& location) const {
    if (const auto it = samples_.find(keyOf(location.instance, location.instanceHigh)); it != samples_.end()) {
        return it->second.front();
    }
    if (location.instanceHigh != 0) {
        if (const auto it = samples_.find(keyOf(location.instance, 0)); it != samples_.end()) {
            return it->second.front();
        }
    }
    return nullptr;
}

std::vector<std::uint8_t> Bank::read(const SampleLocation& location) {
    const content::ResourceIndex::Resource* resource = findSample(location);
    return resource != nullptr ? index_.read(*resource) : std::vector<std::uint8_t>{};
}

std::map<std::string, std::string> Bank::propertySet(std::string_view trackName) {
    const content::ResourceIndex::Resource* resource = find(propertySets_, trackName);
    if (resource == nullptr) {
        return {};
    }
    try {
        return ts2::propset::parse(index_.read(*resource));
    } catch (const std::exception&) {
        return {};
    }
}

} // namespace ts2::audio
