#include "audio/bank.hpp"

#include "audio/audio_config.hpp"
#include "content/install.hpp"
#include "diag/log.hpp"
#include "formats/propset.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <iterator>
#include <set>

namespace ts2::audio {

namespace {

constexpr std::uint32_t kTrackSettings = 0x0B9EB87E;
constexpr std::uint32_t kHitList = 0x7B1ACFCD;
constexpr std::uint32_t kSample = 0x2026960B;
constexpr std::uint32_t kPropertySet = 0xEBFEE33F;

// The sound manager's and the listener's tuning sets in Misc.package.
constexpr SoundKey kManagerTuning{0xFF11CE39, 0x067B4265};
constexpr SoundKey kListenerTuning{0xFFD94C49, 0x104F8C8A};

// The most sound packages and resources read, far above an install's (about 120 packages), so a
// folder of broken files cannot fill the memory.
constexpr std::size_t kMaxPackages = 4096;
constexpr std::size_t kMaxSampleListEntries = 4u << 20;
// The TSAudio files are a few kilobytes.
constexpr std::uintmax_t kMaxIniBytes = 1u << 20;

namespace fs = std::filesystem;

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// A product's TSData folder: the parent chain of one of its object packages up to TSData.
fs::path dataFolder(const content::Product& product) {
    for (const auto& package : product.objectPackages) {
        for (fs::path folder = package.parent_path(); folder.has_relative_path(); folder = folder.parent_path()) {
            if (lower(folder.filename().string()) == "tsdata") {
                return folder;
            }
        }
    }
    return {};
}

std::string readSmallFile(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > kMaxIniBytes) {
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The sound packages of one product, by their path below TSData written as the TSAudio files write
// them (lowercase, forward slashes), sorted.
std::map<std::string, fs::path> productPackages(const fs::path& data) {
    std::map<std::string, fs::path> out;
    std::error_code error;
    for (const auto& item : fs::directory_iterator(data / "Res" / "Sound", error)) {
        if (item.is_regular_file() && lower(item.path().extension().string()) == ".package") {
            out.emplace("res/sound/" + lower(item.path().filename().string()), item.path());
        }
    }
    return out;
}

// What the product's TSAudioPacked*.ini files name, in file order.
std::vector<std::string> namedSegments(const fs::path& data) {
    std::vector<fs::path> inis;
    std::error_code error;
    for (const auto& item : fs::directory_iterator(data / "Sys", error)) {
        const std::string name = lower(item.path().filename().string());
        if (item.is_regular_file() && name.starts_with("tsaudiopacked") && name.ends_with(".ini")) {
            inis.push_back(item.path());
        }
    }
    std::sort(inis.begin(), inis.end());
    std::vector<std::string> out;
    for (const auto& ini : inis) {
        for (auto& path : packedSegments(readSmallFile(ini))) {
            out.push_back(std::move(path));
        }
    }
    return out;
}

} // namespace

void addSegment(KeyMap& map, std::uint32_t package, std::span<const dbpf::IndexEntry> entries, std::uint32_t type) {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->key.type == type) {
            map.try_emplace(mapKey(it->key.instance, it->key.instanceHigh), Location{package, *it});
        }
    }
}

std::vector<fs::path> soundPackageOrder(const fs::path& installRoot) {
    auto products = content::inGameOrder(content::findProducts(installRoot));
    std::reverse(products.begin(), products.end());
    std::vector<std::map<std::string, fs::path>> packages;
    std::vector<fs::path> out;
    std::set<fs::path> taken;
    for (const auto& product : products) {
        const fs::path data = dataFolder(product);
        packages.push_back(data.empty() ? std::map<std::string, fs::path>{} : productPackages(data));
        if (data.empty()) {
            continue;
        }
        for (const auto& named : namedSegments(data)) {
            const auto it = packages.back().find(named);
            if (it != packages.back().end() && taken.insert(it->second).second) {
                out.push_back(it->second);
            }
        }
    }
    for (const auto& product : packages) {
        for (const auto& [name, path] : product) {
            if (taken.insert(path).second) {
                out.push_back(path);
            }
        }
    }
    if (out.size() > kMaxPackages) {
        out.resize(kMaxPackages);
    }
    return out;
}

void Bank::load(const fs::path& installRoot) {
    KeyMap definitions, hitLists;
    for (const auto& path : soundPackageOrder(installRoot)) {
        try {
            auto package = std::make_unique<dbpf::Package>(path);
            if (package->appleDouble()) {
                continue;
            }
            const auto index = static_cast<std::uint32_t>(packages_.size());
            addSegment(samples_, index, package->entries(), kSample);
            addSegment(hitLists, index, package->entries(), kHitList);
            addSegment(definitions, index, package->entries(), kTrackSettings);
            addSegment(propertySets_, index, package->entries(), kPropertySet);
            package->forgetEntries();
            packages_.push_back(std::move(package));
        } catch (const std::exception& ex) {
            diag::warnOnce("sound package " + path.filename().string() + ": " + ex.what());
        }
    }
    readTracks(definitions, hitLists);
    readPropertySets();
    for (auto& package : packages_) {
        package->close();
    }
}

std::vector<std::uint8_t> Bank::read(const Location& location) {
    try {
        return packages_[location.package]->read(location.entry);
    } catch (const std::exception& ex) {
        diag::warnOnce(std::string("sound resource: ") + ex.what());
        return {};
    }
}

void Bank::readTracks(const KeyMap& definitions, const KeyMap& hitLists) {
    // A track is a settings resource, a hit list, or both under one key.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> keys;  // key, first package holding it
    keys.reserve(hitLists.size() + definitions.size());
    for (const auto& [key, at] : hitLists) {
        keys.emplace_back(key, at.package);
    }
    for (const auto& [key, at] : definitions) {
        if (!hitLists.contains(key)) {
            keys.emplace_back(key, at.package);
        }
    }
    // Read package by package, so each file is read front to back once.
    std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second < b.second : a.first < b.first; });
    tracks_.reserve(keys.size());
    for (const auto& [key, package] : keys) {
        Track track;
        if (const auto it = definitions.find(key); it != definitions.end()) {
            try {
                track.settings = trackSettings(propset::parse(read(it->second)));
            } catch (const std::exception&) {
            }
        }
        if (const auto it = hitLists.find(key); it != hitLists.end()) {
            const auto samples = hitListSamples(read(it->second));
            if (sampleLists_.size() + samples.size() <= kMaxSampleListEntries) {
                track.firstSample = static_cast<std::uint32_t>(sampleLists_.size());
                track.sampleCount = static_cast<std::uint32_t>(samples.size());
                sampleLists_.insert(sampleLists_.end(), samples.begin(), samples.end());
            }
        }
        tracks_.emplace(key, track);
        firstOfInstance_.try_emplace(static_cast<std::uint32_t>(key), key);
    }
}

std::map<std::string, std::string> Bank::propertySet(SoundKey key) {
    const auto it = propertySets_.find(mapKey(key.instance, key.instanceHigh));
    if (it == propertySets_.end()) {
        return {};
    }
    try {
        return propset::parse(read(it->second));
    } catch (const std::exception&) {
        return {};
    }
}

void Bank::readPropertySets() {
    for (const auto& [event, list] : propertySet(soundKey("events"))) {
        const SoundKey key = soundKey(event);
        eventMasks_[mapKey(key.instance, key.instanceHigh)] = modifierMask(list);
    }
    aliases_ = aliasMap(propertySet(soundKey("aliases")));
    readManagerTuning(propertySet(kManagerTuning), tuning_);
    readListenerTuning(propertySet(kListenerTuning), tuning_);
}

const Bank::Track* Bank::track(SoundKey key) const {
    std::uint64_t at = mapKey(key.instance, key.instanceHigh);
    if (key.instanceHigh == 0 && !tracks_.contains(at)) {
        const auto first = firstOfInstance_.find(key.instance);
        if (first == firstOfInstance_.end()) {
            return nullptr;
        }
        at = first->second;
    }
    const auto it = tracks_.find(at);
    return it == tracks_.end() ? nullptr : &it->second;
}

std::span<const SampleKey> Bank::samples(const Track& track) const {
    return std::span<const SampleKey>(sampleLists_).subspan(track.firstSample, track.sampleCount);
}

std::vector<std::uint8_t> Bank::readSample(SampleKey key) {
    const auto it = samples_.find(mapKey(key.instance, key.instanceHigh));
    return it == samples_.end() ? std::vector<std::uint8_t>{} : read(it->second);
}

std::uint32_t Bank::listedMask(std::string_view event) const {
    const SoundKey key = soundKey(event);
    const auto it = eventMasks_.find(mapKey(key.instance, key.instanceHigh));
    return it == eventMasks_.end() ? 0 : it->second;
}

} // namespace ts2::audio
