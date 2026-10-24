#include "content/lighting_state.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace ts2::content {

namespace {

bool sameName(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// The day's state: Seasons' weather and season, or a vacation lot's kind without it.
std::string dayState(const Lighting& lighting, const LightingConditions& c) {
    std::string name = "day";
    if (c.season) {
        if (c.overcast) {
            name = "overcast";
        } else if (*c.season == 0) {
            name = "spring";
        } else if (*c.season == 2) {
            name = "autumn";
        } else if (*c.season == 3) {
            name = "winter";
        }
    } else if (c.vacation && c.vacationLotKind == 5) {
        name = "spring";
    } else if (c.vacation && c.vacationLotKind == 6) {
        name = "autumn";
    }
    return findState(lighting, name) ? name : "day";
}

} // namespace

const Lighting::State* findState(const Lighting& lighting, const std::string& name, std::string* key) {
    if (const auto exact = lighting.states.find(name); exact != lighting.states.end()) {
        if (key) *key = exact->first;
        return &exact->second;
    }
    for (const auto& [stateName, state] : lighting.states) {
        if (sameName(stateName, name)) {
            if (key) *key = stateName;
            return &state;
        }
    }
    return nullptr;
}

StateTime stateTime(const std::string& state) {
    constexpr std::string_view kNoPortals = "NoPortals";
    std::string name = state;
    if (name.size() > kNoPortals.size() && sameName(name.substr(name.size() - kNoPortals.size()), std::string(kNoPortals))) {
        name.resize(name.size() - kNoPortals.size());
    }
    for (const char* night : {"night", "demoNight", "neighborhoodnight", "vacationNight"}) {
        if (sameName(name, night)) {
            return StateTime::Night;
        }
    }
    if (sameName(name, "morning")) {
        return StateTime::Morning;
    }
    if (sameName(name, "evening")) {
        return StateTime::Evening;
    }
    return StateTime::Day;
}

std::string lotSkybox(StateTime time) {
    return time == StateTime::Night ? "skymap-generic-night-envcube" : "skymap-generic-day-highres-envcube";
}

std::string lotLightingState(const Lighting& lighting, int timeOfDay, const LightingConditions& c) {
    const auto setting = lighting.values.find("morningEvening");
    const bool morningEvening = setting == lighting.values.end() || !(sameName(setting->second, "false") || setting->second == "0");
    std::string name = "day";
    bool day = false;
    switch (timeOfDay) {
    case 0:
        day = true;
        break;
    case 1:
        day = !morningEvening;
        name = morningEvening ? "evening" : name;
        break;
    case 2:
        name = "night";
        break;
    case 3:
        name = morningEvening ? "morning" : "night";
        break;
    default:
        break;
    }
    if (day) {
        name = dayState(lighting, c);
    } else if (c.vacation) {
        name = "vacationNight";
    }
    std::string key;
    if (!c.portalLighting && findState(lighting, name + "NoPortals", &key)) {
        return key;
    }
    return findState(lighting, name, &key) ? key : std::string();
}

} // namespace ts2::content
