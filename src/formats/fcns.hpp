#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::fcns {

// A list of named tuning values (FCNS): the game reads its autonomy tunings ("random selection
// count", "high attenuation"...) from one, by name, and keeps its built-in value for a missing name.
struct Tunings {
    std::string name;
    std::uint32_t version = 0;
    struct Value {
        std::string name;
        float value = 0;
    };
    std::vector<Value> values;

    // The first value with this name.
    [[nodiscard]] std::optional<float> find(std::string_view valueName) const;
};

Tunings parse(std::span<const std::uint8_t> data);

} // namespace ts2::fcns
