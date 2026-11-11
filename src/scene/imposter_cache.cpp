#include "scene/imposter_cache.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

namespace ts2::scene {

namespace {

constexpr std::array<char, 8> kMagic{'N', 'E', 'O', 'I', 'M', 'P', 'S', 'T'};
// Raised whenever the bake draws differently, so that older pictures are made again.
constexpr std::uint32_t kVersion = 6;
// Limits that keep a broken file from asking for much: more textures than a lot's imposter has,
// longer names, or sides past what High draws.
constexpr std::uint32_t kMostTextures = 512;
constexpr std::size_t kLongestName = 64;
constexpr std::uint32_t kLargestSide = 2048;

std::size_t dxt5Bytes(std::uint32_t width, std::uint32_t height) {
    return std::size_t{(width + 3) / 4} * ((height + 3) / 4) * 16;
}

template <typename T>
bool readValue(std::istream& in, T& value) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof value));
}

template <typename T>
void writeValue(std::ostream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof value);
}

} // namespace

std::optional<ImposterCacheKey> imposterCacheKey(const std::filesystem::path& lotPackage) {
    std::error_code error;
    const auto size = std::filesystem::file_size(lotPackage, error);
    if (error) {
        return std::nullopt;
    }
    const auto time = std::filesystem::last_write_time(lotPackage, error);
    if (error) {
        return std::nullopt;
    }
    return ImposterCacheKey{size, static_cast<std::int64_t>(time.time_since_epoch().count())};
}

std::filesystem::path imposterCacheFile(const std::filesystem::path& cacheFolder, const std::filesystem::path& lotPackage) {
    return cacheFolder / (lotPackage.stem().native() + std::filesystem::path(".neoimposter").native());
}

std::optional<std::map<std::string, Image>> readImposterCache(const std::filesystem::path& file, const ImposterCacheKey& key) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::array<char, 8> magic{};
    std::uint32_t version = 0, count = 0;
    ImposterCacheKey saved;
    if (!in.read(magic.data(), magic.size()) || magic != kMagic || !readValue(in, version) || version != kVersion ||
        !readValue(in, saved.packageSize) || !readValue(in, saved.packageTime) || !(saved == key) || !readValue(in, count) || count > kMostTextures) {
        return std::nullopt;
    }
    std::map<std::string, Image> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint8_t length = 0;
        if (!readValue(in, length) || length == 0 || length > kLongestName) {
            return std::nullopt;
        }
        std::string name(length, '\0');
        Image image;
        std::uint32_t bytes = 0;
        if (!in.read(name.data(), length) || !readValue(in, image.width) || !readValue(in, image.height) || !readValue(in, image.blockFormat) ||
            !readValue(in, bytes) || image.width == 0 || image.height == 0 || image.width > kLargestSide || image.height > kLargestSide ||
            image.blockFormat != kDxt5 || bytes != dxt5Bytes(image.width, image.height)) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> blocks(bytes);
        if (!in.read(reinterpret_cast<char*>(blocks.data()), bytes)) {
            return std::nullopt;
        }
        image.blockLevels.push_back(std::move(blocks));
        out[std::move(name)] = std::move(image);
    }
    return out;
}

bool writeImposterCache(const std::filesystem::path& file, const ImposterCacheKey& key, const std::map<std::string, Image>& pictures) {
    if (pictures.size() > kMostTextures) {
        return false;
    }
    for (const auto& [name, image] : pictures) {
        if (name.empty() || name.size() > kLongestName || image.blockFormat != kDxt5 || image.blockLevels.empty() || image.width > kLargestSide ||
            image.height > kLargestSide || image.blockLevels.front().size() != dxt5Bytes(image.width, image.height)) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    // Written beside it first, so that a run stopped halfway leaves no broken file in its place.
    // Two bakers may finish the same lot, so each writes its own.
    static std::atomic<std::uint32_t> writes{0};
    std::filesystem::path writing = file;
    writing += "." + std::to_string(writes.fetch_add(1)) + ".writing";
    {
        std::ofstream out(writing, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(kMagic.data(), kMagic.size());
        writeValue(out, kVersion);
        writeValue(out, key.packageSize);
        writeValue(out, key.packageTime);
        writeValue(out, static_cast<std::uint32_t>(pictures.size()));
        for (const auto& [name, image] : pictures) {
            writeValue(out, static_cast<std::uint8_t>(name.size()));
            out.write(name.data(), static_cast<std::streamsize>(name.size()));
            writeValue(out, image.width);
            writeValue(out, image.height);
            writeValue(out, image.blockFormat);
            writeValue(out, static_cast<std::uint32_t>(image.blockLevels.front().size()));
            out.write(reinterpret_cast<const char*>(image.blockLevels.front().data()), static_cast<std::streamsize>(image.blockLevels.front().size()));
        }
        if (!out) {
            out.close();
            std::filesystem::remove(writing, error);
            return false;
        }
    }
    std::filesystem::rename(writing, file, error);
    if (error) {
        std::filesystem::remove(writing, error);
        return false;
    }
    return true;
}

} // namespace ts2::scene
