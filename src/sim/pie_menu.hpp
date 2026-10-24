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

// The interaction table an object's menu comes from: the one its definition names (field 7), from
// its own group, else its semi-global group, returned with the group it was found in. A multi-tile part
// whose definition names 0xFFFF uses its master's.
struct ObjectInteractions {
    std::shared_ptr<const ttab::InteractionTable> table;
    std::uint32_t group = 0;
};
ObjectInteractions interactionsOf(CodeSource& code, const SimObject& object);

// What a Sim queues for a choice of `object`'s menu, at the priority the game gives a menu choice.
[[nodiscard]] std::optional<QueuedInteraction> queuedChoice(CodeSource& code, const SimObject& object, const PieChoice& choice);

// The text of interaction `index` of `object`'s menu, as the pie menu shows it ("?" and the index
// when the object has no such line).
[[nodiscard]] std::string interactionName(CodeSource& code, const SimObject& object, std::uint16_t index);

} // namespace ts2::sim
