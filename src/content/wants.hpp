#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ts2::content {

// What a want's argument refers to. The numbers are the ones saved with each want.
enum class WantArgument : std::uint8_t { None = 0, Sim = 1, Object = 2, Category = 3, Skill = 4, Career = 5, Badge = 6 };

// How a want compares the value it is satisfied with against its own value.
enum class WantOperator : std::uint8_t {
    None = 0,  // always matches
    Equals = 1,
    LessThan = 2,
    GreaterThan = 3,
    LessThanOrEquals = 4,
    GreaterThanOrEquals = 5,
};

// How big a want is: Power wants and fears show a burst behind their icon.
enum class WantLevel : std::uint8_t { Transitory = 0, Memorable = 1, Power = 2 };

struct WantDefinition {
    std::uint32_t guid = 0;
    std::string name;
    WantArgument argument = WantArgument::None;
    WantOperator op = WantOperator::None;
    std::int32_t score = 0;  // aspiration points
    // Tree that lists the Sims the want is checked for; empty when it is just the Sim's own.
    std::string simArrayTree;
    std::int32_t influence = 0;
    WantLevel level = WantLevel::Transitory;
    // Its pictures in the UI images (group 0x499DB772): the icon and a small badge; 0 for none.
    std::uint32_t primaryIcon = 0, secondaryIcon = 0;
};

struct WantDefinitions {
    std::map<std::uint32_t, WantDefinition> byGuid;
    std::vector<std::string> problems;
};

// The newest Wants.package of the install (each product ships a cumulative copy; the largest
// is the newest), if any.
std::optional<std::filesystem::path> wantsPackage(const std::filesystem::path& installRoot);

WantDefinitions loadWantDefinitions(const std::filesystem::path& wantsPackage);

} // namespace ts2::content
