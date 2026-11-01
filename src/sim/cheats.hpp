#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::sim {

class World;

// The lot the cheats act on: its objects and the simulator globals (the clock, the funds...).
struct CheatLot {
    World& world;
    std::span<std::int16_t> globals;
};

// The game's on/off properties that cheats set with boolProp. Names are compared without case, so
// "testingcheatsenabled" is the same property as "testingCheatsEnabled". A property never set is off.
class CheatProperties {
public:
    [[nodiscard]] bool get(std::string_view name) const;
    void set(std::string_view name, bool value);

    // testingCheatsEnabled: with it on, a Shift-click opens an object's menu of debug lines (interaction
    // table flag 0x80) instead of its usual menu. The game reads it each time, so it takes effect at once.
    [[nodiscard]] bool testingCheats() const { return get("testingCheatsEnabled"); }

private:
    struct NoCase {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const;
    };
    std::map<std::string, bool, NoCase> values_;
};

// What a line typed into the cheat window did.
struct CheatResult {
    // False for an unknown command or bad arguments. Then, and after help, the game opens the window's
    // output part. After any other line the window closes unless its output part is open.
    bool ok = false;
    std::vector<std::string> output;  // lines for the output part
    enum class Window { None, Expand, Clear, Hide };
    Window window = Window::None;  // what the line asks of the window itself
};

// Runs one line of the cheat window as the game's cheat system does. Command names are compared
// without case. The commands kept so far: boolProp <name> <on|off>, help, the window's own expand,
// clear and hide, and on a lot maxMotives, motiveDecay <on|off>, lockAspiration <on|off> and
// setHour <0-23>. help lists every command, as its pattern and -all aren't kept yet. `lot` is null
// outside a lot, and then the lot's commands do nothing.
CheatResult runCheat(CheatProperties& properties, CheatLot* lot, std::string_view line);

} // namespace ts2::sim
