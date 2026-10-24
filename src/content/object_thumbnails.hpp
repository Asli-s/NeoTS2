#pragma once

#include "formats/dbpf.hpp"
#include "formats/tga.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

namespace ts2::content {

// The object pictures the game draws itself (for objects whose package has no catalog picture) and
// caches in the user folder (Thumbnails/ObjectThumbnails.package). A picture is filed under the
// object's group (high instance) and a CRC of the group and the object's model name (low instance).
class ObjectThumbnails {
public:
    // A missing or broken file has no pictures.
    explicit ObjectThumbnails(const std::filesystem::path& file);

    // The low instance the game files the picture of the object in `group` with model `modelName`
    // under. Nullopt for objects the game never caches (no model name, or the model "embedded").
    [[nodiscard]] static std::optional<std::uint32_t> key(std::uint32_t group, std::string_view modelName);

    // The cached picture of the object in `group` with model `modelName`, or nullopt when there is none.
    [[nodiscard]] std::optional<tga::Pixels> picture(std::uint32_t group, std::string_view modelName, std::uint32_t maxSide);

private:
    std::optional<dbpf::Package> package_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> entries_;  // by low and high instance
};

} // namespace ts2::content
