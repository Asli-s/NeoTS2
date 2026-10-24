#include "formats/nhood_paint_rules.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace ts2::nhood {

namespace {

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    return s.substr(i);
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream in(s);
    while (std::getline(in, item, ',')) {
        out.push_back(trim(item));
    }
    return out;
}

long number(const std::string& s) { return std::strtol(s.c_str(), nullptr, 0); }
float decimal(const std::string& s) { return std::strtof(s.c_str(), nullptr); }
float positiveOr(float value, float fallback) { return std::isfinite(value) && value > 0 ? value : fallback; }

} // namespace

PaintRules parsePaintRules(std::string_view text) {
    PaintRules rules;
    std::istringstream in{std::string(text)};
    std::string section;
    for (std::string line; std::getline(in, line);) {
        if (const auto semi = line.find(';'); semi != std::string::npos) {
            line.resize(semi);
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[') {
            section = line.substr(1, line.find(']') - 1);
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = trim(line.substr(0, eq));
        const auto values = split(line.substr(eq + 1));
        if (section == "PaintInfos" && values.size() >= 4) {
            PaintInfo p;
            p.maxStrength = decimal(values[0]);
            p.texture = values[1];
            // Variants are named with a two-digit suffix, so there are at most 100.
            p.variants = static_cast<int>(std::clamp(number(values[2]), 1L, 100L));
            p.lotPaint = values[3];
            for (std::size_t i = 4; i + 1 < values.size(); i += 2) {
                p.spread.emplace_back(decimal(values[i]), decimal(values[i + 1]));
            }
            rules.paints[static_cast<std::uint8_t>(number(key))] = std::move(p);
        } else if (section == "TerrainTypeInfos" && values.size() >= 2) {
            auto& t = rules.types[key];
            t.cliff = static_cast<std::uint8_t>(number(values[0]));
            t.beach = static_cast<std::uint8_t>(number(values[1]));
        } else if (section.rfind("PaintMapTable_", 0) == 0) {
            auto& t = rules.types[section.substr(14)];
            const long row = number(key);
            if (row >= 0 && row < 32) {
                for (std::size_t c = 0; c < 16 && c < values.size(); ++c) {
                    t.table[static_cast<std::size_t>(row)][c] = static_cast<std::uint8_t>(number(values[c]));
                }
            }
        } else if (section == "NHoodTerrainTunables" && !values.empty()) {
            if (key == "MaxBeachAltitude") rules.maxBeachAltitude = decimal(values[0]);
            else if (key == "MaxBeachWidthInCells") rules.maxBeachWidthInCells = static_cast<int>(std::clamp(number(values[0]), 0L, 253L));
            else if (key == "HeightRangeForTemperatureDistribution") rules.heightRangeForTemperature = positiveOr(decimal(values[0]), rules.heightRangeForTemperature);
            else if (key == "CliffAngleMin") rules.cliffAngleMin = decimal(values[0]);
            else if (key == "CliffAngleMax") rules.cliffAngleMax = decimal(values[0]);
            else if (key == "WaterDepthThresholdForPaint") rules.waterDepthThresholdForPaint = decimal(values[0]);
            else if (key == "MaxRoadProximityValue") rules.maxRoadProximity = static_cast<int>(std::clamp(number(values[0]), 1L, 255L));
        } else if (section == "NHoodTerrainRenderTunables" && key == "TextureVariantGridSize" && !values.empty()) {
            rules.textureVariantGridSize = static_cast<int>(number(values[0]));
        }
    }
    return rules;
}

double spreadAt(const PaintInfo& paint, float distance) {
    const auto& s = paint.spread;
    if (s.empty()) {
        return 0;
    }
    if (distance <= s.front().first) {
        return s.front().second;
    }
    for (std::size_t i = 1; i < s.size(); ++i) {
        if (distance < s[i].first) {
            const auto& a = s[i - 1];
            const auto& b = s[i];
            // The game keeps each piece's slope as a float and works the rest out in more precision.
            const auto slope = static_cast<float>((b.second - a.second) / (b.first - a.first));
            return (static_cast<double>(distance) - a.first) * slope + a.second;
        }
    }
    return s.back().second;
}

} // namespace ts2::nhood
