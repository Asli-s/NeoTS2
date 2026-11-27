#include "content/object_thumbnails.hpp"

#include "diag/log.hpp"
#include "formats/dbpf_append.hpp"
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

ObjectThumbnails::ObjectThumbnails(const std::filesystem::path& file) : file_(file) {
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
    return low ? pictureAt(group, *low, maxSide) : std::nullopt;
}

std::uint32_t ObjectThumbnails::colourKey(std::uint32_t group, std::string_view model, std::string_view subset, std::string_view material) {
    std::string text = std::to_string(static_cast<std::int32_t>(group));
    std::string m;
    for (const char c : model) {
        m += lowered(c);
    }
    constexpr std::string_view kCres = "_cres";
    if (m.size() > kCres.size() && m.ends_with(kCres)) {
        m.resize(m.size() - kCres.size());
    }
    text += m;
    for (const std::string_view part : {subset, material}) {
        for (const char c : part) {
            text += lowered(c);
        }
    }
    return hash::crc32(text);
}

std::optional<tga::Pixels> ObjectThumbnails::pictureAt(std::uint32_t group, std::uint32_t low, std::uint32_t maxSide) {
    if (!package_) {
        return std::nullopt;
    }
    // Pictures the game cached earlier on were filed with high instance 0.
    auto found = entries_.find({low, group});
    if (found == entries_.end()) {
        found = entries_.find({low, 0u});
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

bool ObjectThumbnails::store(std::uint32_t group, std::uint32_t low, const tga::Pixels& picture) {
    std::error_code ec;
    if (file_.empty() || !std::filesystem::is_regular_file(file_, ec)) {
        return false;
    }
    const auto jpeg = image::encodeJpegWithAlpha(picture);
    if (jpeg.empty()) {
        return false;
    }
    const dbpf::StoredResource stored{{kObjectThumbnail, 0xFFFFFFFF, low, group}, jpeg, false, static_cast<std::uint32_t>(jpeg.size())};
    if (package_) {
        package_->close();
    }
    try {
        dbpf::appendToPackage(file_, std::span(&stored, 1));
    } catch (const std::exception& e) {
        diag::warnOnce(std::string("thumbnails: a drawn picture can't be kept: ") + e.what());
        return false;
    }
    *this = ObjectThumbnails(file_);
    return true;
}

} // namespace ts2::content
