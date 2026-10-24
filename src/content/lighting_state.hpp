#pragma once

#include "content/lighting.hpp"

#include <optional>
#include <string>

namespace ts2::content {

// What besides the clock decides a lot's lighting state.
struct LightingConditions {
    // Seasons' weather, when the game has it: the season (0 spring, 1 summer, 2 autumn, 3 winter)
    // and whether the sky is overcast.
    std::optional<int> season;
    bool overcast = false;
    // On a vacation lot (Bon Voyage). Without Seasons' weather, vacation lots of kind 5 and 6 use
    // the spring and autumn states. What those kinds are is not known yet.
    bool vacation = false;
    int vacationLotKind = 0;
    // The game's portal lighting (off at the Medium lighting option uses the "...NoPortals" states).
    bool portalLighting = true;
};

// A lighting state by name, any case (the files write "Winter", the game asks for "winter").
const Lighting::State* findState(const Lighting& lighting, const std::string& name, std::string* key = nullptr);

// The name of the lighting state the game lights a lot with at `timeOfDay` (the Time Of Day global:
// 0 day, 1 evening, 2 night, 3 morning), as written in the light files. Empty when the light files
// have none of the states it could pick. With "morningEvening" off (as shipped), morning is lit as
// night and evening as day.
std::string lotLightingState(const Lighting& lighting, int timeOfDay, const LightingConditions& conditions = {});

// The part of the day a lighting state belongs to, as the game sorts states for its sky and
// effects: "night", "demoNight", "neighborhoodnight" and "vacationNight" are night, "morning" and
// "evening" their own, and every other state day. A "...NoPortals" state goes with its own.
enum class StateTime { Morning, Day, Evening, Night };
StateTime stateTime(const std::string& state);

// The skybox texture behind a lot (the lot sky effects' plain-weather skyboxes). Seasons and weather
// pick other day skies, and the morning and evening skies are not known yet (they are the day's).
std::string lotSkybox(StateTime time);

} // namespace ts2::content
