#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string_view>
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
    // Tree that lists the Sims the want is checked for, empty when it is just the Sim's own.
    std::string simArrayTree;
    std::int32_t influence = 0;
    WantLevel level = WantLevel::Transitory;
    // Its pictures in the UI images (group 0x499DB772): the icon and a small badge, 0 for none.
    std::uint32_t primaryIcon = 0, secondaryIcon = 0;
    // Its texts: string table (STR#) `stringSet` of Text\Wants.package. Rows 1 and 2 are its name
    // aimed at a male or female target, 3 its name without one, 4 its description.
    std::uint32_t stringSet = 0;
};

// A category of objects wants can be about (the objects' OBJD field 0x2F), with its pictures.
struct WantCategory {
    std::uint32_t primaryIcon = 0, secondaryIcon = 0;
};

// A tuning curve: straight between its points (by rising x) and flat past its ends, as the game
// evaluates its curves.
struct TuningCurve {
    std::vector<std::pair<float, float>> points;
    [[nodiscard]] float at(float x) const;
    // The same points with x and y swapped (the game keeps the influence curve both ways).
    [[nodiscard]] TuningCurve inverse() const;
};

struct WantDefinitions {
    std::map<std::uint32_t, WantDefinition> byGuid;
    std::map<std::uint32_t, WantCategory> categories;  // by category id
    // The influence tuning curve inverted: influence level by influence points.
    std::optional<TuningCurve> influenceLevels;
    std::vector<std::string> problems;
};

// The newest Wants.package of the install, if any. Each product ships a cumulative copy, and the
// largest is the newest.
[[nodiscard]] std::optional<std::filesystem::path> wantsPackage(const std::filesystem::path& installRoot);

WantDefinitions loadWantDefinitions(const std::filesystem::path& wantsPackage);

// The newest WantTuning.package of the install (cumulative like Wants.package), if any.
[[nodiscard]] std::optional<std::filesystem::path> wantTuningPackage(const std::filesystem::path& installRoot);

// Reads the object categories of a WantTuning text (its `<category id=... primaryIcon=...
// secondaryIcon=.../>` entries). Entries it can't read are skipped.
std::map<std::uint32_t, WantCategory> parseWantCategories(std::string_view tuning);

// Reads the influence curve of a WantTuning text (`<influenceTuning curve = "(0;0) (1;2000) ..."/>`,
// points by level). None when it is missing or has a point it can't read.
std::optional<TuningCurve> parseInfluenceCurve(std::string_view tuning);

// The text of a WantTuning package, empty when it can't be read.
std::string loadWantTuning(const std::filesystem::path& tuningPackage);

// What the influence bar shows (0..100) for a Sim's influence points (person data 0xB2): its level
// on `influenceLevels` times 0.2 (points / 75 without a curve), kept to 0..1, times 100 and cut to
// a whole number, as the game's wants manager works it out.
int influenceMeterValue(const TuningCurve* influenceLevels, std::int16_t points);

} // namespace ts2::content
