#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace ts2::weather {

constexpr std::uint32_t kType = 0xB21BE28B;

// A neighborhood's weather and season (Seasons), as scripts read them through WeatherInfo: a
// version, then (version 3 on) the record's name, then the values below in this order. The pond
// value is there only from version 2 on. A version 0 record holds nothing and means spring-like defaults.
struct Weather {
    enum Value {
        kSeason,
        kSeasonQuadrant,
        kSeasonDaysStandard,
        kSeasonDaysAdditional,
        kPrecipitationType,
        kPrecipitationIntensity,
        kAccumulationLevel,
        kAccumulationType,
        kTemperature,
        kSkyClarity,
        kPondFrozenState,
        kValueCount
    };
    std::array<std::int32_t, kValueCount> values{1};
};

Weather parse(std::span<const std::uint8_t> data);

} // namespace ts2::weather
