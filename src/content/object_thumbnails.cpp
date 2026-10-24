#include "content/object_thumbnails.hpp"

#include "formats/hash.hpp"
#include "formats/image.hpp"

#include <string>

namespace ts2::content {

namespace {

constexpr std::uint32_t kObjectThumbnail = 0xAC2950C1;

char lowered(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

ObjectThumbnails::ObjectThumbnails(const std::filesystem::path& file) {
    std::error_code ec;
    if (file.empty() || !std::filesystem::is_regular_file(file, ec)) {
        return;
    }
    try {
        const auto& entries = package_.emplace(file).entries();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& k = entries[i].key;
            if (k.type == kObjectThumbnail) {
                entries_.emplace(std::pair{k.instance, k.instanceHigh}, i);
            }
        }
    } catch (const std::exception&) {
        entries_.clear();
        package_.reset();
    }
}

std::optional<std::uint32_t> ObjectThumbnails::key(std::uint32_t group, std::string_view modelName) {
    std::string lowName;
    for (const char c : modelName) {
        lowName += lowered(c);
    }
    if (lowName.empty() || lowName == "embedded") {
        return std::nullopt;
    }
    return hash::crc32(std::to_string(static_cast<std::int32_t>(group)) + lowName);
}

std::optional<tga::Pixels> ObjectThumbnails::picture(std::uint32_t group, std::string_view modelName, std::uint32_t maxSide) {
    const auto low = key(group, modelName);
    if (!low || !package_) {
        return std::nullopt;
    }
    // Pictures the game cached earlier on were filed with high instance 0.
    auto found = entries_.find({*low, group});
    if (found == entries_.end()) {
        found = entries_.find({*low, 0u});
    }
    if (found == entries_.end()) {
        return std::nullopt;
    }
    try {
        return image::decode(package_->read(package_->entries().at(found->second)), maxSide);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace ts2::content
