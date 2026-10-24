#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ts2::nhood {

// One terrain paint from TSData/Sys/NeighborhoodTerrain.ini.
struct PaintInfo {
    float maxStrength = 0;
    std::string texture;  // the neighborhood texture's name, without the variant number
    int variants = 1;
    std::string lotPaint;
    std::vector<std::pair<float, float>> spread;  // (distance, strength) points of the spread curve
};

// A terrain type's paints: the cliff and beach paints and, by temperature row (0 the highest
// ground) and road proximity column, the paint table.
struct TerrainTypeInfo {
    std::uint8_t cliff = 0, beach = 0;
    std::array<std::array<std::uint8_t, 16>, 32> table{};
};

struct PaintRules {
    std::map<std::uint8_t, PaintInfo> paints;
    std::map<std::string, TerrainTypeInfo> types;  // by name, as the terrain names its type
    // [NHoodTerrainTunables], holding the shipped file's values until read.
    float maxBeachAltitude = 4;
    int maxBeachWidthInCells = 2;
    float heightRangeForTemperature = 500;
    float cliffAngleMin = 52, cliffAngleMax = 56;  // degrees
    float waterDepthThresholdForPaint = 50;
    int maxRoadProximity = 20;
    int textureVariantGridSize = 4;  // [NHoodTerrainRenderTunables], 4 is the game's own default
};

PaintRules parsePaintRules(std::string_view text);

// The spread curve's strength at a distance: straight lines between its points, holding the first
// and last values beyond them.
double spreadAt(const PaintInfo& paint, float distance);

} // namespace ts2::nhood
