#include "audio/sound_bank.hpp"

#include "content/install.hpp"
#include "diag/log.hpp"
#include "formats/hash.hpp"
#include "formats/propset.hpp"

#include <algorithm>
#include <cctype>
#include <format>

namespace ts2::audio {

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kClipType = 0x2026960B;
constexpr std::uint32_t kTrackType = 0x0B9EB87E;
constexpr std::uint32_t kHitListType = 0x7B1ACFCD;
constexpr std::uint32_t kPropertySetType = 0xEBFEE33F;
constexpr std::uint32_t kAudioSettingsGroup = 0x4C1940C5;
constexpr std::uint32_t kAliasInstance = 0xFF1EBA43, kAliasHigh = 0x3CF64D4B;

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// HitLab's track properties, by key (0xFF000000 | crc24 of the property's name in HitLab.ini).
namespace property {
constexpr std::string_view kPitch = "0xff099db2", kVolumeInternal = "0xff53dbf3", kIs3d = "0xffcc156f",
                           kMinimumDistance = "0xffaab623", kMaximumDistance = "0xffa6a614", kRolloff = "0xff0150f2",
                           kLooped = "0xfff7b8df", kFadeIn = "0xffff4e79", kFadeOut = "0xff595ffd", kPriority = "0xff050a94",
                           kPan = "0xff3ec880", kPitchRandomMin = "0xff8dcfd8", kPitchRandomMax = "0xff69510b",
                           kSelfInterrupting = "0xff303b1b";
}

// The TSData folders of an install's products, oldest product first, so later ones win.
std::vector<fs::path> dataFolders(const fs::path& install) {
    auto products = content::findProducts(install);
    std::stable_sort(products.begin(), products.end(), [](const auto& a, const auto& b) { return a.releaseOrder < b.releaseOrder; });
    std::vector<fs::path> folders;
    for (const auto& product : products) {
        for (const auto& objects : product.objectPackages) {
            fs::path data = objects.parent_path();
            while (data.has_relative_path() && lower(data.filename().string()) != "tsdata") {
                data = data.parent_path();
            }
            if (data.has_relative_path() && std::find(folders.begin(), folders.end(), data) == folders.end()) {
                folders.push_back(data);
            }
        }
    }
    return folders;
}

Category categoryOf(const fs::path& package) {
    const std::string name = lower(package.stem().string());
    if (name.starts_with("voice")) {
        return Category::Voice;
    }
    return Category::Effects;
}

} // namespace

SoundBank::HitList::HitList(std::vector<ClipKey> clips) : available_(std::move(clips)), window_(available_.size() / 3 + 1) {}

ClipKey SoundBank::HitList::choose(std::mt19937& random) {
    ClipKey picked;
    if (available_.empty()) {
        if (recent_.empty()) {
            return picked;
        }
        picked = recent_.front();
        recent_.erase(recent_.begin());
        recent_.push_back(picked);
        return picked;
    }
    std::uniform_int_distribution<std::size_t> any(0, available_.size() - 1);
    const std::size_t at = any(random);
    picked = available_[at];
    available_.erase(available_.begin() + static_cast<std::ptrdiff_t>(at));
    recent_.push_back(picked);
    while (recent_.size() > window_) {
        available_.push_back(recent_.front());
        recent_.erase(recent_.begin());
    }
    return picked;
}

TrackSettings SoundBank::readTrack(std::span<const std::uint8_t> propertySet) {
    std::map<std::string, std::string> p;
    for (auto& [k, v] : propset::parse(propertySet)) {
        p[lower(k)] = v;  // keys are written in lowercase hex, but nothing promises it
    }
    TrackSettings t;
    const auto has = [&](std::string_view name) { return p.contains(std::string(name)); };
    const auto get = [&](std::string_view name) { return propset::number(p, std::string(name)); };
    const auto signedGet = [&](std::string_view name) { return static_cast<std::int32_t>(get(name)); };
    if (has(property::kPitch)) {
        t.pitchHz = static_cast<int>(std::min<std::uint32_t>(get(property::kPitch), 192000));
    }
    if (has(property::kVolumeInternal)) {
        t.volume = static_cast<float>(std::min<std::uint32_t>(get(property::kVolumeInternal), 4096)) / 1024.0f;
    }
    t.is3d = get(property::kIs3d) != 0;
    if (has(property::kMinimumDistance)) {
        t.minDistance = static_cast<float>(std::clamp<std::uint32_t>(get(property::kMinimumDistance), 1, 1000));
    }
    if (has(property::kMaximumDistance)) {
        t.maxDistance = std::max(t.minDistance, static_cast<float>(std::min<std::uint32_t>(get(property::kMaximumDistance), 10000)));
    }
    if (has(property::kRolloff)) {
        t.rolloff = static_cast<float>(std::min<std::uint32_t>(get(property::kRolloff), 100));
        t.rolloff = t.rolloff <= 0 ? 1 : t.rolloff;
    }
    t.looped = get(property::kLooped) != 0;
    if (has(property::kFadeIn)) {
        t.fadeInMs = static_cast<int>(std::min<std::uint32_t>(get(property::kFadeIn), 60000));
    }
    if (has(property::kFadeOut)) {
        t.fadeOutMs = static_cast<int>(std::min<std::uint32_t>(get(property::kFadeOut), 60000));
    }
    if (has(property::kPriority)) {
        t.priority = static_cast<int>(std::min<std::uint32_t>(get(property::kPriority), 127));
    }
    if (has(property::kPan)) {
        t.pan = (static_cast<float>(std::min<std::uint32_t>(get(property::kPan), 1024)) - 512.0f) / 512.0f;
    }
    t.pitchRandomMin = std::clamp(signedGet(property::kPitchRandomMin), -60, 60);
    t.pitchRandomMax = std::clamp(signedGet(property::kPitchRandomMax), -60, 60);
    t.selfInterrupting = static_cast<int>(std::min<std::uint32_t>(get(property::kSelfInterrupting), 3));
    return t;
}

std::vector<ClipKey> SoundBank::readHitList(std::span<const std::uint8_t> bytes) {
    std::vector<ClipKey> clips;
    if (bytes.size() < 8) {
        return clips;
    }
    const auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1]) << 8 |
               static_cast<std::uint32_t>(bytes[at + 2]) << 16 | static_cast<std::uint32_t>(bytes[at + 3]) << 24;
    };
    const std::uint32_t version = u32(0);
    const std::size_t room = bytes.size() - 8;
    if (version == 0x38) {
        const std::size_t count = std::min<std::size_t>(u32(4), room / 8);
        for (std::size_t i = 0; i < count; ++i) {
            clips.push_back({u32(8 + 8 * i), u32(12 + 8 * i)});
        }
    } else if (version == 1) {
        for (std::size_t i = 0; i < room / 4; ++i) {
            clips.push_back({u32(8 + 4 * i), 0});
        }
    }
    return clips;
}

SoundBank::SoundBank(const fs::path& install) {
    std::optional<Where> aliases;
    for (const fs::path& data : dataFolders(install)) {
        std::error_code error;
        std::vector<fs::path> found;
        for (auto it = fs::recursive_directory_iterator(data / "Res" / "Sound", error); !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
            if (it->is_regular_file(error) && lower(it->path().extension().string()) == ".package") {
                found.push_back(it->path());
            }
        }
        std::sort(found.begin(), found.end());
        for (const fs::path& path : found) {
            if (packages_.size() >= 0xFFFF) {
                break;
            }
            try {
                auto file = std::make_unique<dbpf::Package>(path);
                const auto index = static_cast<std::uint16_t>(packages_.size());
                for (const dbpf::IndexEntry& e : file->entries()) {
                    const Where where{index, e};
                    const std::uint64_t k = key(e.key.instance, e.key.instanceHigh);
                    if (e.key.type == kClipType) {
                        clips_[k] = where;
                        clipByInstance_[e.key.instance] = k;
                        if ((e.key.group & 0xFF000000u) == 0xFF000000u) {
                            stations_[e.key.group].push_back({e.key.instance, e.key.instanceHigh});
                        }
                    } else if (e.key.type == kTrackType) {
                        tracks_[k] = where;
                    } else if (e.key.type == kHitListType) {
                        hitLists_[k] = where;
                    } else if (e.key.type == kPropertySetType && e.key.group == kAudioSettingsGroup && e.key.instance == kAliasInstance &&
                               e.key.instanceHigh == kAliasHigh) {
                        aliases = where;
                    }
                }
                file->forgetEntries();
                packages_.push_back({path, categoryOf(path), std::move(file)});
            } catch (const std::exception& e) {
                diag::warning(std::format("sound: {} can't be read: {}", path.filename().string(), e.what()));
            }
        }
    }
    if (aliases) {
        try {
            for (const auto& [from, to] : propset::parse(read(*aliases))) {
                aliases_[lower(from)] = lower(to);
            }
        } catch (const std::exception& e) {
            diag::warning(std::format("sound: the alias table can't be read: {}", e.what()));
        }
    }
    diag::info(std::format("sound: {} packages, {} tracks, {} hit lists, {} clips, {} aliases", packages_.size(), tracks_.size(),
                           hitLists_.size(), clips_.size(), aliases_.size()));
}

std::vector<std::uint8_t> SoundBank::read(const Where& where) {
    Package& p = packages_.at(where.package);
    auto bytes = p.file->read(where.entry);
    p.file->close();  // hundreds of packages: none is kept open
    return bytes;
}

bool isVoiceName(std::string_view name) {
    return name.size() > 3 && lower(name.substr(0, 3)) == "vox";
}

std::vector<std::string> voiceNames(std::string_view name, const SimVoice& voice) {
    const std::string base = lower(name) + "_";
    const char sex = voice.female ? 'f' : 'm';
    const char letter = voice.voice == 'b' ? 'b' : 'a';
    std::vector<std::string> names;
    const auto withSex = [&](char age) {
        names.push_back(base + age + sex + letter);
        if (letter != 'a') {
            names.push_back(base + age + sex + 'a');
        }
    };
    const auto noSex = [&](char age) {
        names.push_back(base + age + letter);
        if (letter != 'a') {
            names.push_back(base + age + 'a');
        }
    };
    switch (voice.age) {
    case SimVoice::Age::Adult: withSex('a'); break;
    case SimVoice::Age::Teen: withSex('t'); withSex('a'); break;
    case SimVoice::Age::Elder: withSex('e'); withSex('a'); break;
    case SimVoice::Age::Child: noSex('c'); break;
    case SimVoice::Age::Toddler: noSex('p'); noSex('c'); break;
    case SimVoice::Age::Baby: noSex('b'); noSex('p'); noSex('c'); break;
    }
    return names;
}

bool SoundBank::has(std::string_view name) const {
    std::string lowered = lower(name);
    if (const auto alias = aliases_.find(lowered); alias != aliases_.end()) {
        lowered = alias->second;
    }
    const auto hashed = hash::scenegraphKey(lowered);
    const std::uint64_t k = key(hashed.instance, hashed.instanceHigh);
    return tracks_.contains(k) && hitLists_.contains(k);
}

std::optional<PickedSound> SoundBank::pick(std::string_view name) {
    std::string lowered = lower(name);
    if (const auto alias = aliases_.find(lowered); alias != aliases_.end()) {
        lowered = alias->second;
    }
    const auto hashed = hash::scenegraphKey(lowered);
    const std::uint64_t k = key(hashed.instance, hashed.instanceHigh);
    const auto track = tracks_.find(k);
    const auto hits = hitLists_.find(k);
    if (track == tracks_.end() || hits == hitLists_.end()) {
        return std::nullopt;
    }
    PickedSound picked;
    ClipKey clipKey;
    try {
        const std::lock_guard lock(mutex_);
        auto chosen = chosen_.find(k);
        if (chosen == chosen_.end()) {
            chosen = chosen_.emplace(k, HitList(readHitList(read(hits->second)))).first;
        }
        clipKey = chosen->second.choose(random_);
        picked.name = std::string(name);
        picked.track = readTrack(read(track->second));
        picked.category = packages_.at(track->second.package).category;
    } catch (const std::exception& e) {
        diag::warnOnce(std::format("sound: \"{}\" can't be read: {}", lowered, e.what()));
        return std::nullopt;
    }
    picked.pcm = clip(clipKey);  // locks on its own, and not while it decodes
    if (!picked.pcm) {
        return std::nullopt;
    }
    return picked;
}

std::vector<ClipKey> SoundBank::station(std::string_view name) const {
    const auto found = stations_.find(0xFF000000u | hash::crc24(lower(name)));
    return found != stations_.end() ? found->second : std::vector<ClipKey>{};
}

std::shared_ptr<const Pcm> SoundBank::clip(const ClipKey& clipKey) {
    std::uint64_t k = key(clipKey.instance, clipKey.instanceHigh);
    if (clipKey.instanceHigh == 0 && !clips_.contains(k)) {
        if (const auto full = clipByInstance_.find(clipKey.instance); full != clipByInstance_.end()) {
            k = full->second;
        }
    }
    std::unique_lock lock(mutex_);
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
        if (it->first == k) {
            cache_.splice(cache_.begin(), cache_, it);
            return it->second;
        }
    }
    const auto where = clips_.find(k);
    if (where == clips_.end()) {
        return nullptr;
    }
    std::vector<std::uint8_t> bytes;
    try {
        bytes = read(where->second);
    } catch (const std::exception& e) {
        diag::warnOnce(std::format("sound: a clip can't be read: {}", e.what()));
        return nullptr;
    }
    lock.unlock();  // decoding a song takes a while; others may pick meanwhile
    auto decoded = decodeClip(bytes);
    if (!decoded) {
        return nullptr;
    }
    auto pcm = std::make_shared<const Pcm>(std::move(*decoded));
    lock.lock();
    cache_.emplace_front(k, pcm);
    cacheBytes_ += pcm->samples.size() * sizeof(std::int16_t);
    while (cacheBytes_ > kCacheBytes && cache_.size() > 1) {
        cacheBytes_ -= cache_.back().second->samples.size() * sizeof(std::int16_t);
        cache_.pop_back();
    }
    return pcm;
}

} // namespace ts2::audio
