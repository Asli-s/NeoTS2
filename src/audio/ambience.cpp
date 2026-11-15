#include "audio/ambience.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <string_view>

namespace ts2::audio {

namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> out;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view word = trim(text.substr(0, comma));
        if (!word.empty()) {
            out.emplace_back(word);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return out;
}

std::vector<std::pair<float, float>> points(std::string_view text) {
    const auto numbers = words(text);
    std::vector<std::pair<float, float>> out;
    for (std::size_t i = 0; i + 1 < numbers.size(); i += 2) {
        out.emplace_back(static_cast<float>(std::atof(numbers[i].c_str())), static_cast<float>(std::atof(numbers[i + 1].c_str())));
    }
    return out;
}

// Straight between points, held at the ends.
float curveAt(const std::vector<std::pair<float, float>>& curve, float at) {
    if (curve.empty()) {
        return 1;
    }
    if (at <= curve.front().first) {
        return curve.front().second;
    }
    for (std::size_t i = 1; i < curve.size(); ++i) {
        if (at <= curve[i].first) {
            const auto& [a, va] = curve[i - 1];
            const auto& [b, vb] = curve[i];
            return b > a ? va + (vb - va) * (at - a) / (b - a) : vb;
        }
    }
    return curve.back().second;
}

const std::vector<std::pair<float, float>>* findCurve(const AmbienceLayer& layer, const std::string& name) {
    for (const auto& [curve, values] : layer.curves) {
        if (curve == name) {
            return &values;
        }
    }
    return nullptr;
}

bool partitionHolds(std::string_view id, const AmbienceState& state) {
    if (id == "isflora") {
        return state.flora;
    }
    if (id == "iswater") {
        return state.water;
    }
    if (id == "isinside") {
        // shortcut: one open fraction for the whole camera area, upgrade when areas are partitioned.
        return state.outside < 0.5f;
    }
    if (id == "istombstone") {
        return state.tombstone;
    }
    // Unknown conditions (weather, season) keep the layer silent.
    return false;
}

float layerVolume(const AmbienceLayer& layer, const AmbienceState& state) {
    float volume = 1;
    for (std::size_t i = 0; i < layer.enabled.size(); ++i) {
        const std::string& id = layer.enabled[i];
        if (id == "islot") {
            if (!state.onLot) {
                return 0;
            }
        } else if (id == "isneighborhood") {
            if (!state.inNeighborhood) {
                return 0;
            }
        } else if (id == "lightofday") {
            // The next primitive names the time-of-day curve.
            if (i + 1 < layer.enabled.size()) {
                if (const auto* curve = findCurve(layer, layer.enabled[i + 1])) {
                    volume *= curveAt(*curve, static_cast<float>(state.hour / 24.0));
                }
                ++i;
            }
        } else if (const auto* curve = findCurve(layer, id)) {
            volume *= curveAt(*curve, static_cast<float>(state.hour / 24.0));
        } else {
            // Unknown conditions (weather, season) keep the layer silent.
            return 0;
        }
    }
    for (const std::string& id : layer.andIds) {
        if (!partitionHolds(id, state)) {
            return 0;
        }
    }
    for (const std::string& id : layer.notIds) {
        if (partitionHolds(id, state)) {
            return 0;
        }
    }
    // Loop volume is pairs of value primitive and curve.
    for (std::size_t i = 0; i + 1 < layer.loopVolumes.size(); i += 2) {
        const std::string& value = layer.loopVolumes[i];
        const float at = value == "outsideness" ? state.outside : value == "zoom" ? state.zoom : 1;
        if (const auto* curve = findCurve(layer, layer.loopVolumes[i + 1])) {
            volume *= curveAt(*curve, at);
        }
    }
    return std::clamp(volume, 0.0f, 1.0f);
}

} // namespace

std::optional<AmbienceLayer> ambienceLayer(const std::map<std::string, std::string>& properties) {
    const auto find = [&properties](std::string_view key) -> std::optional<std::string> {
        const auto it = properties.find(std::string(key));
        return it == properties.end() ? std::nullopt : std::optional(it->second);
    };
    const auto name = find("Name");
    if (!name || name->empty()) {
        return std::nullopt;
    }
    AmbienceLayer layer;
    layer.name = *name;
    if (const auto enabled = find("EnabledPrimitiveIds")) {
        layer.enabled = words(*enabled);
    }
    if (const auto ids = find("ANDPartitionPrimitiveIds")) {
        layer.andIds = words(*ids);
    }
    if (const auto ids = find("NOTPartitionPrimitiveIds")) {
        layer.notIds = words(*ids);
    }
    if (const auto volumes = find("LoopVolumePrimitives")) {
        layer.loopVolumes = words(*volumes);
    }
    if (const auto loop = find("AmbientLoopName")) {
        layer.loop = *loop;
    }
    if (const auto freshness = find("FreshnessScoreName")) {
        layer.freshness = *freshness;
    }
    // Any setting that is a list of number pairs is a curve.
    for (const auto& [key, value] : properties) {
        const auto curve = points(value);
        if (curve.size() >= 2) {
            layer.curves.emplace_back(key, curve);
        }
    }
    return layer;
}

std::vector<AmbiencePlay> activeLayers(const std::vector<AmbienceLayer>& layers, const AmbienceState& state) {
    std::vector<AmbiencePlay> out;
    for (const AmbienceLayer& layer : layers) {
        // Note-sequence-only layers have nothing to loop yet.
        if (layer.loop.empty()) {
            continue;
        }
        const float volume = layerVolume(layer, state);
        if (volume > 0.01f) {
            out.push_back({layer.name, layer.loop, volume});
        }
    }
    return out;
}

} // namespace ts2::audio
