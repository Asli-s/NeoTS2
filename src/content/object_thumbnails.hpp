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

    // The low instance of a colour choice's picture in the game's cache of them
    // (Thumbnails/DesignModeThumbnails.package, read with this class too): the same CRC of the group,
    // then the model name without "_cres", the subset and the material, all lowercase.
    [[nodiscard]] static std::uint32_t colourKey(std::uint32_t group, std::string_view model, std::string_view subset, std::string_view material);
    // The cached picture filed under `group` and `low`, or nullopt.
    [[nodiscard]] std::optional<tga::Pixels> pictureAt(std::uint32_t group, std::uint32_t low, std::uint32_t maxSide);

    // Adds `picture` to the file under `group` and `low`, replacing any there, as the game files the
    // pictures it draws. False when the file is missing or can't be written.
    bool store(std::uint32_t group, std::uint32_t low, const tga::Pixels& picture);

private:
    std::filesystem::path file_;
    std::optional<dbpf::Package> package_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> entries_;  // by low and high instance
};

} // namespace ts2::content
