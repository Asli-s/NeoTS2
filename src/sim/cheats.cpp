#include "sim/cheats.hpp"

#include "sim/clock.hpp"
#include "sim/world.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <iterator>
#include <cctype>
#include <format>
#include <optional>

namespace ts2::sim {

namespace {

char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool sameNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

// A typed line is far shorter than this. Longer lines are not run, so a pasted file can't fill memory.
constexpr std::size_t kMaxLine = 4096;

// The line's words, split at spaces and tabs. A word in double quotes may hold spaces.
std::vector<std::string_view> words(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        if (i == line.size()) {
            break;
        }
        if (line[i] == '"') {
            const std::size_t end = line.find('"', i + 1);
            const std::size_t stop = end == std::string_view::npos ? line.size() : end;
            out.push_back(line.substr(i + 1, stop - i - 1));
            i = end == std::string_view::npos ? line.size() : end + 1;
            continue;
        }
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        out.push_back(line.substr(start, i - start));
    }
    return out;
}

struct Command {
    std::string_view name;
    // The game's help text. One starting with '*' is hidden from help.
    std::string_view help;
};

// The game's own help texts, sorted by name as help lists them.
constexpr Command kCommands[] = {
    {"boolProp", "*<name:string> <value:bool>"},
    {"clear", "*Clears the text in the cheat window."},
    {"expand", "Expands or contracts the output portion of the cheat console."},
    {"help", "[wildcard] [-all]  Logs this help output."},
    {"hide", "*Hides the cheat window."},
    {"lockAspiration", "lockAspiration [on/off]: Locks/Unlocks the aspiration score of sims on the lot."},
    {"maxMotives", "Sets all the motives to maximum for all sims on the lot"},
    {"motiveDecay", "motiveDecay [on/off]: Turns motive decay on/off for all sims on the lot "},
    {"setHour", "[0-23] sets the hour of the day"},
};

// Person data 0x1D: 2 stops a Sim's motives from decaying and from moving by their motive changes.
constexpr std::size_t kMotivesPaused = 0x1D;
// Person data 0xC1: 1 keeps the Sim's aspiration score where it is.
constexpr std::size_t kAspirationLocked = 0xC1;
// maxMotives leaves these motives alone and sets every other of the 16 to 100.
constexpr std::size_t kMotivesKept[] = {2, 10, 11, 12, 13};
// Setting the time also sets global 0x1D to 30, whose meaning is not known yet.
constexpr std::size_t kTimeSetGlobal = 0x1D;
constexpr std::int16_t kTimeSetValue = 30;

// Each command whose help text isn't hidden, as "name: text" with the name padded to 20. The game
// logs them in its cheat system's group, so each line shows after "TSCheatSystem: ".
std::vector<std::string> helpLines() {
    std::vector<std::string> out;
    for (const auto& command : kCommands) {
        if (!command.help.starts_with('*')) {
            out.push_back(std::format("TSCheatSystem: {:<20}: {}", command.name, command.help));
        }
    }
    return out;
}

// An on/off argument. The game's script parser names true and on 1, false and off 0. Other
// spellings, such as numbers, aren't known to work in the game yet.
std::optional<bool> boolValue(std::string_view word) {
    if (sameNoCase(word, "true") || sameNoCase(word, "on")) {
        return true;
    }
    if (sameNoCase(word, "false") || sameNoCase(word, "off")) {
        return false;
    }
    return std::nullopt;
}

// Each person on the lot, as the lot's cheats go over them.
template <class F>
void forEachPerson(World& lot, F visit) {
    for (SimObject* sim = lot.nextAfter(INT16_MIN, [](const SimObject& o) { return o.isPerson(); }); sim != nullptr;
         sim = lot.nextAfter(sim->id, [](const SimObject& o) { return o.isPerson(); })) {
        visit(*sim);
    }
}

// motiveDecay and lockAspiration: an on/off word after the name sets a person data field on every
// person on the lot. Without one the game does nothing, and the line still counts as done.
bool setOnLot(CheatLot* lot, const std::vector<std::string_view>& args, std::size_t field, std::int16_t whenOn, std::int16_t whenOff) {
    if (args.size() < 2) {
        return true;
    }
    const auto on = boolValue(args[1]);
    if (!on) {
        return false;
    }
    if (lot != nullptr) {
        forEachPerson(lot->world, [&](SimObject& sim) { sim.personData[field] = *on ? whenOn : whenOff; });
    }
    return true;
}

} // namespace

bool CheatProperties::NoCase::operator()(std::string_view a, std::string_view b) const {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) { return lower(x) < lower(y); });
}

bool CheatProperties::get(std::string_view name) const {
    const auto found = values_.find(name);
    return found != values_.end() && found->second;
}

void CheatProperties::set(std::string_view name, bool value) {
    values_.insert_or_assign(std::string(name), value);
}

CheatResult runCheat(CheatProperties& properties, CheatLot* lot, std::string_view line) {
    CheatResult result;
    if (line.size() > kMaxLine) {
        return result;
    }
    const auto args = words(line);
    if (args.empty()) {
        result.ok = true;
        return result;
    }
    const std::string_view name = args[0];
    if (sameNoCase(name, "boolProp")) {
        // Only the form that sets a property is kept. The one that reads it isn't known yet.
        if (args.size() == 3) {
            if (const auto value = boolValue(args[2])) {
                properties.set(args[1], *value);
                result.ok = true;
            }
        }
        return result;
    }
    if (sameNoCase(name, "help")) {
        result.ok = true;
        result.output = helpLines();
        return result;
    }
    if (sameNoCase(name, "expand") || sameNoCase(name, "clear") || sameNoCase(name, "hide")) {
        result.ok = true;
        result.window = sameNoCase(name, "expand")  ? CheatResult::Window::Expand
                        : sameNoCase(name, "clear") ? CheatResult::Window::Clear
                                                    : CheatResult::Window::Hide;
        return result;
    }
    if (sameNoCase(name, "maxMotives")) {
        result.ok = true;
        if (lot != nullptr) {
            forEachPerson(lot->world, [](SimObject& sim) {
                for (std::size_t motive = 0; motive < kMotiveCount; ++motive) {
                    if (std::ranges::find(kMotivesKept, motive) == std::end(kMotivesKept)) {
                        sim.motives[motive] = 100.0f;
                    }
                }
            });
        }
        return result;
    }
    if (sameNoCase(name, "motiveDecay")) {
        result.ok = setOnLot(lot, args, kMotivesPaused, 0, 2);
        return result;
    }
    if (sameNoCase(name, "setHour")) {
        // The hour must be digits alone. What the game makes of other words is not known yet.
        if (args.size() < 2) {
            result.ok = true;
            return result;
        }
        const std::string_view word = args[1];
        if (word.empty() || word.size() > 9 || !std::ranges::all_of(word, [](char c) { return c >= '0' && c <= '9'; })) {
            return result;
        }
        result.ok = true;
        const int hour = std::stoi(std::string(word));
        // The hour and minute 0 are set, the rest of the date is left as it is.
        if (hour < 24 && lot != nullptr && lot->globals.size() > kTimeSetGlobal) {
            lot->globals[global::kHour] = static_cast<std::int16_t>(hour);
            lot->globals[global::kMinute] = 0;
            lot->globals[kTimeSetGlobal] = kTimeSetValue;
            updateTimeOfDay(lot->globals);
        }
        return result;
    }
    if (sameNoCase(name, "lockAspiration")) {
        result.ok = setOnLot(lot, args, kAspirationLocked, 1, 0);
        return result;
    }
    result.output.emplace_back("Unknown command");
    return result;
}

} // namespace ts2::sim
