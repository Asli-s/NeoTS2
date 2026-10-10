#pragma once

#include "content/resource_index.hpp"
#include "formats/tga.hpp"
#include "formats/ui_script.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ts2::content {

// The game's user interface data, all products loaded in release order (a later product's resource
// replaces an earlier one's): the UI scripts and their images (TSData/Res/UI/ui.package) and the
// text packages their captions come from (TSData/Res/Text, each filed under the group named after
// it, e.g. Neighborhood 0x7F3A262A).
class UiData {
public:
    explicit UiData(const std::filesystem::path& install);

    // A UI script's windows; empty when it is missing.
    std::optional<std::vector<ui::Window>> script(std::uint32_t group, std::uint32_t instance);
    // An image a script names as {group, instance}; empty when it is missing or unreadable.
    std::optional<tga::Pixels> image(std::uint32_t group, std::uint32_t instance);
    // The text a script names as captionres={group, value}: string (value & 0xFFFF), counted from
    // 1, of string table (value >> 16) in the text package with that group, in `language` (1 is US
    // English); empty when there is none.
    std::string text(std::uint32_t group, std::uint32_t value, std::uint8_t language = 1);

    std::size_t packageCount() const { return index_.packageCount(); }

private:
    ResourceIndex index_;
};

} // namespace ts2::content
