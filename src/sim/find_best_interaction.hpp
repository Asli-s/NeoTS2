#pragma once

#include "formats/fcns.hpp"
#include "formats/ttab.hpp"
#include "sim/code_source.hpp"
#include "sim/sim_random.hpp"
#include "sim/world.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ts2::sim {

// The tunings autonomy reads from the game's tuning list (see CodeSource::tunings), by their names
// there. A missing name keeps the game's built-in value.
struct AutonomyTunings {
    float familyMinimum = 0.2f;   // min autonomy score for family
    float visitorMinimum = 0.05f; // min autonomy score for visitors
    float sittingMinimum = 0.1f;  // min autonomy score for sitting
    int randomSelection = 10;     // random selection count
    int worstMotives = 3;         // random worst motive count
    int maxChecks = 200;          // max check tree in fba count
    float floorCost = 10;         // autonomy floor to floor distance cost
    // Low, moderate and high attenuation, for residents and for visitors.
    std::array<float, 3> attenuation{0.002f, 0.02f, 0.1f};
    std::array<float, 3> visitorAttenuation{0.002f, 0.02f, 0.1f};

    static AutonomyTunings from(const fcns::Tunings* tunings);
};

// How much an interaction's appeal falls with distance, from its attenuation code: 0 the line's own
// value, 1 none, 2-4 low, moderate and high (a visitor's moderate and high are its own). Any
// other code gives 0.02.
float attenuationOf(const ttab::Interaction& line, const AutonomyTunings& tunings, bool visitor);

// A candidate Find Best Interaction scored.
struct FoundInteraction {
    std::int16_t object = 0;
    std::uint16_t interaction = 0;  // the line's id (string index)
    float score = 0;
    float after = 0;       // the Sim's happiness after the line's advertised changes
    bool checked = false;  // its check tree ran before it was scored, and passed
    InteractionIcon icon;  // its queue icon as the line and that check tree left it
    // A line a check tree offered (see AutonomyOffer): its name and the params its tree runs with
    // (0 for the line itself).
    std::string name;
    std::array<std::int16_t, 4> params{};
};

// A line a check tree offers with Add/Change the Action String while autonomy checks a candidate
// before scoring it: weighed beside the candidate, with the line as the tree had changed it then.
// It is not checked again.
struct AutonomyOffer {
    std::string name;
    std::array<std::int16_t, 4> params{};
    InteractionIcon icon;
    ttab::Interaction line;
};

// Find Best Interaction (0x03, Sims only): what a Sim would most like to do by itself. Operands:
//   bytes 0-1  the motives to look for (bit n = motive n), 0 for the Sim's lowest motives
//   byte 3     0x01 the motives given are for the first pass only, then the lowest motives follow,
//              0x02 only objects in the room in temp 0, 0x20 distances from the object in temp 1
//
// It runs at most once a tick. Each pass takes motives (the Sim's `random worst motive count`
// lowest that some object advertises, unless given) and looks at the objects advertising them: those
// placed, not in use (lockouts, interactions disabled, placement flag 2 bit 4) and not skipped by
// object data 0x38. Each of their advertising lines the Sim's age may use, not a debug line and
// with an autonomy level up to the Sim's (person data 0x24), becomes a candidate, named by its id
// (string index). A line on the Sim itself needs flag 0x100000. A candidate's score is how much
// happier its advertisements would make the Sim, divided by (attenuation * distance + 1), distance
// in tiles with a floor counting as `floor cost`. Scores below the Sim's minimum (or 0.00001) are
// dropped. Check trees that change the line run before scoring. Others run only when the candidate
// comes up for the pick. The lines a check tree offers before scoring (see AutonomyOffer) are scored
// after each pass's candidates, all of them again in every pass. The pick is the first of the best
// that passes its check (one of the best `random selection count`, at random). Passes go on while
// nothing was picked or fewer than `max check tree in fba count` check trees ran, until no motive
// is left.
//
// Not yet: the rooms the Sim can reach, the interactions of other Sims it could join, autonomy
// memory, ownership and social factors, and pets.
struct InteractionSearch {
    World& world;
    CodeSource& code;
    SimObject& me;
    std::span<const std::uint8_t> operands;
    std::span<const std::int16_t> temps;
    const AutonomyTunings& tunings;
    SimRandom& rng;
    // Runs a line's check tree for autonomy, with `object` as the stack object. True when it passes.
    // `changed` receives the line as the tree left it (check trees can write its advertisements),
    // `icon` the candidate's queue icon as the tree left it, `offers` the lines it offered.
    std::function<bool(SimObject& object, const ttab::Interaction& line, ttab::Interaction& changed, InteractionIcon& icon,
                       std::vector<AutonomyOffer>& offers)>
        check;
    // Whether a line's check tree has to run before the line is scored.
    std::function<bool(const SimObject& object, const ttab::Interaction& line)> checkFirst;
    // If set, receives every candidate scored, best first (for tests and debugging).
    std::vector<FoundInteraction>* scored = nullptr;
};

// Which check trees change the interaction they test, and so run before their line is scored: those
// that write its advertisements (an assigning Expression on owner 0x0C, 0x1C or 0x1D), run Lua, set
// the action's name (Add/Change the Action String) or run a functional tree or a tree by name,
// themselves or in a tree they call. Each tree is looked at once.
class CheckTreeKinds {
public:
    explicit CheckTreeKinds(CodeSource& code) : code_(code) {}
    bool changesLine(std::uint32_t codeOwner, std::uint16_t tree) { return visit(codeOwner, tree, 0); }

private:
    bool visit(std::uint32_t codeOwner, std::uint16_t tree, int depth);

    CodeSource& code_;
    std::map<std::pair<std::uint32_t, std::uint16_t>, bool> known_;
};

// The pick, or nullopt when the Sim already looked this tick or nothing qualifies.
[[nodiscard]] std::optional<FoundInteraction> findBestInteraction(const InteractionSearch& search);

} // namespace ts2::sim
