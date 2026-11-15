#pragma once


#include "audio/track.hpp"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ts2::audio {

struct AmbienceLayer {
    std::string name;
    std::vector<std::string> enabled;       // its EnabledPrimitiveIds
    std::vector<std::string> andIds, notIds;  // its AND and NOT partitions
    std::vector<std::string> loopVolumes;   // its LoopVolumePrimitives: value, curve, value, curve...
    // Named curves: (at, value) points over 0 to 1.
    std::vector<std::pair<std::string, std::vector<std::pair<float, float>>>> curves;
    std::string loop;       // AmbientLoopName, empty for a layer that only has a sequence
    // shortcut: the note sequences (FreshnessScoreName) are not played yet, upgrade when FSC is in.
    std::string freshness;  // FreshnessScoreName: its note sequence, not played yet
};

struct AmbienceState {
    bool onLot = false;
    bool inNeighborhood = false;
    double hour = 12;      // 0 to 24
    float outside = 1;     // 0 inside the house .. 1 out in the open around the camera
    float zoom = 1;        // 0 far .. 1 near
    bool water = false;
    bool flora = true;
    bool tombstone = false;
};

struct AmbiencePlay {
    std::string layer;  // the layer's name
    std::string loop;   // the track's name
    float volume = 1;
};

// The layers whose conditions hold, with their levels. Layers with no loop are left out for now.
std::vector<AmbiencePlay> activeLayers(const std::vector<AmbienceLayer>& layers, const AmbienceState& state);

// Nothing when the set has no Name.
std::optional<AmbienceLayer> ambienceLayer(const std::map<std::string, std::string>& properties);

} // namespace ts2::audio
