#pragma once

#include "sim/code_source.hpp"
#include "sim/vm.hpp"
#include "sim/world.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ts2::sim {

// One line of a pie menu.
struct PieChoice {
    std::string name;  // '/' separates sub-menu levels
    std::uint16_t interaction = 0;  // the id (string index) of the interaction table line it runs
    std::array<std::int16_t, 4> params{};
    OfferedAction::Disabled disabled = OfferedAction::Disabled::No;
    std::int16_t order = 0;  // higher first
    InteractionIcon icon;    // its queue icon, as the line and its check tree left it
};

struct PieMenuOptions {
    bool debug = false;     // shift-click with testing cheats on: only the debug lines
    bool allMenus = false;  // the allMenus cheat: every line passes its check tree
    bool firstOnly = false; // stop at the first line offered (whether the object can be used)
};

struct PieMenu {
    std::vector<PieChoice> choices;
    std::vector<std::string> problems;  // check trees that stopped without an answer
};

// What `sim` may do with `object`, as the game's pie menu offers it: each line of the object's
// interaction table that passes the game's checks and its check tree, highest order first. The
// join lines and the cheat menus are not offered yet.
PieMenu pieMenu(Vm& vm, CodeSource& code, SimObject& sim, SimObject& object, const PieMenuOptions& options = {});

// The checks the game gives a waiting interaction's line just before it starts, before its check
// tree: who may use it (age and species, visitors), whether `object` takes interactions (not burning,
// not turned off, and with `autonomy` its lockout, object data 0x19, at 0), parental control, and
// not a debug line. The game lets debug lines start from a shift-click menu, which isn't kept yet.
[[nodiscard]] bool mayStartLine(const ttab::Interaction& line, const SimObject& sim, const SimObject& object,
                                bool parentalControl, bool autonomy);

// The interaction table an object's menu comes from: the one its definition names (field 7), from
// its own group, else its semi-global group, returned with the group it was found in. A multi-tile part
// whose definition names 0xFFFF uses its master's.
struct ObjectInteractions {
    std::shared_ptr<const ttab::InteractionTable> table;
    std::uint32_t group = 0;
};
ObjectInteractions interactionsOf(CodeSource& code, const SimObject& object);

// What a Sim queues for a choice of `object`'s menu, at the priority the game gives a menu choice.
// It runs on the part World::interactionPart gives.
[[nodiscard]] std::optional<QueuedInteraction> queuedChoice(CodeSource& code, const World& world, const SimObject& object,
                                                            const PieChoice& choice);

// Whether a choice of `object`'s menu runs at once when picked instead of waiting in the Sim's queue
// (interaction table flag 0x4, as the lights' Turn On and Turn Off have).
[[nodiscard]] bool runsAtOnce(CodeSource& code, const SimObject& object, const PieChoice& choice);

// Runs such a choice as the game does when it is picked: its line is checked again and dropped
// (False) when it is no longer offered. Otherwise its action tree runs to its end at once on top
// of whatever `sim` is doing, as `sim`, with the part as its stack object. Meanwhile person data
// 0x21 holds the choice's priority and the Sim counts as not autonomous (person data 0xA6 and its
// running interaction). Those and the Sim's temps are put back afterwards.
RunResult runAtOnce(Vm& vm, CodeSource& code, World& world, SimObject& sim, SimObject& object, const PieChoice& choice);

// The text of interaction `index` of `object`'s menu, as the pie menu shows it ("?" and the index
// when the object has no such line).
[[nodiscard]] std::string interactionName(CodeSource& code, const SimObject& object, std::uint16_t index);

} // namespace ts2::sim
