#pragma once

#include "sim/code_source.hpp"
#include "sim/world.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::sim {

// A way between levels over stairs or another portal object (a pool ladder, a diving board), as the
// game's route planner links its level grids. Each end is a bone routing slot of a part, named in the
// type's "Model - Bones" string table: "<name>[,<target>]". A slot without the target flag (bit 21)
// pairs with the slot whose name has the opposite suffix (_top and _bottom, _n and _s...). Crossing
// it runs the part's Portal function (OBJf entry 15) on the Sim, which climbs and changes its level.
struct StairPortal {
    struct End {
        float x = 0, y = 0;  // tiles
        int level = 1;       // 1 = ground
        std::int16_t part = 0;
        std::int32_t slot = 0;  // the part's routing slot
        std::optional<std::int16_t> facing;  // the slot's: a Sim arriving to climb faces this way
    };
    End from, to;
    bool twoWay = false;      // false: only from `from` to `to`
    std::int16_t owner = 0;   // the stair's lead part
};

// The name a stair slot pairs with: its last suffix turned around (_n and _s, _e and _w, _ne and _sw,
// _nw and _se, _top and _bottom), empty when it has none of them.
std::string partnerSlotName(std::string_view name);

// Every portal of the lot's stairs (OBJD types 10 to 12) and of its other objects with a Portal
// function, doors aside. Parts of one object (the same lead, object data 0x13) give their slots in
// id order, a modular stair's top part (type 12) followed by the last part down its chain (object
// data 0x50). A name met again is skipped. Ends whose bone the model lacks are left out.
std::vector<StairPortal> stairPortals(const World& world, CodeSource& code);

// The ways through a door (OBJD type 8) whose lead part is `lead`, paired from its parts' slots as a
// stair's are. A door's two ends stand either side of its wall.
std::vector<StairPortal> doorPortals(const World& world, CodeSource& code, const SimObject& lead);

} // namespace ts2::sim
